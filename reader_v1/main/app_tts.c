#include "app_tts.h"
#include "board_pins.h"
#include "xl9555.h"
#include "es8388.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_tts.h"
#include "esp_task_wdt.h"

/* Xiaole voice data from .dat file */
extern const uint8_t _binary_esp_tts_voice_data_xiaole_dat_start[];
extern const uint8_t _binary_esp_tts_voice_data_xiaole_dat_end[];

/* Xiaole voice template (struct defined in esp-sr library) */
extern const esp_tts_voice_t esp_tts_voice_xiaole;

static const char *TAG = "TTS";

static i2s_chan_handle_t s_tx_handle = NULL;
static volatile bool s_speaking = false;
static volatile bool s_stop_requested = false;
static esp_tts_handle_t s_tts = NULL;
static esp_tts_voice_t *s_voice = NULL;
static bool s_tts_ready = false;

/* ── Volume & Speed (persistent, adjustable at runtime) ─────────── */
static int s_volume_db = -12;   /* dB, range -48 ~ 0 */
static int s_speed     =  2;    /* 0=slowest, 5=fastest */

/* ── Persistent worker ────────────────────────────────────────────── */

#define TTS_WORKER_STACK   49152
#define TTS_WORKER_PRIO    2
#define TTS_STOP_WAIT_MS   500
#define TTS_STOP_POLL_MS   10
#define TTS_SAFE_TEXT_MAX  96
#define TTS_SAFE_SPEAKABLE_MAX 18
#define TTS_PARSE_FRAGMENT_CHARS 4
#define TTS_RECREATE_INTERVAL 1

typedef struct {
    char text[512];
    tts_done_cb_t cb;
} tts_work_t;

static QueueHandle_t s_work_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static uint32_t s_utt_count = 0;

static esp_err_t tts_recreate_if_needed(void)
{
    if (!s_voice) return ESP_ERR_INVALID_STATE;
    if ((s_utt_count % TTS_RECREATE_INTERVAL) != 0) return ESP_OK;

    esp_tts_handle_t new_tts = esp_tts_create(s_voice);
    if (!new_tts) {
        ESP_LOGE(TAG, "esp_tts_create failed during periodic recreate");
        return ESP_FAIL;
    }

    if (s_tts) {
        esp_tts_destroy(s_tts);
    }
    s_tts = new_tts;
    ESP_LOGI(TAG, "TTS engine recreated (utterances=%u)", (unsigned)s_utt_count);
    return ESP_OK;
}

static size_t utf8_encode_cp(char *dst, size_t dst_cap, uint32_t cp)
{
    if (cp <= 0x7F) {
        if (dst_cap < 1) return 0;
        dst[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        if (dst_cap < 2) return 0;
        dst[0] = (char)(0xC0 | ((cp >> 6) & 0x1F));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        if (dst_cap < 3) return 0;
        dst[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        if (dst_cap < 4) return 0;
        dst[0] = (char)(0xF0 | ((cp >> 18) & 0x07));
        dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

static bool utf8_decode_next(const char *s, size_t len, size_t *ioff, uint32_t *out_cp)
{
    size_t i = *ioff;
    if (i >= len) return false;

    unsigned char b0 = (unsigned char)s[i++];
    if (b0 < 0x80) {
        *out_cp = b0;
        *ioff = i;
        return true;
    }

    if ((b0 & 0xE0) == 0xC0) {
        if (i >= len) return false;
        unsigned char b1 = (unsigned char)s[i];
        if ((b1 & 0xC0) != 0x80) return false;
        i++;
        *out_cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
        *ioff = i;
        return true;
    }

    if ((b0 & 0xF0) == 0xE0) {
        if (i + 1 >= len) return false;
        unsigned char b1 = (unsigned char)s[i];
        unsigned char b2 = (unsigned char)s[i + 1];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
        i += 2;
        *out_cp = ((uint32_t)(b0 & 0x0F) << 12) |
                  ((uint32_t)(b1 & 0x3F) << 6) |
                  (uint32_t)(b2 & 0x3F);
        *ioff = i;
        return true;
    }

    if ((b0 & 0xF8) == 0xF0) {
        if (i + 2 >= len) return false;
        unsigned char b1 = (unsigned char)s[i];
        unsigned char b2 = (unsigned char)s[i + 1];
        unsigned char b3 = (unsigned char)s[i + 2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
        i += 3;
        *out_cp = ((uint32_t)(b0 & 0x07) << 18) |
                  ((uint32_t)(b1 & 0x3F) << 12) |
                  ((uint32_t)(b2 & 0x3F) << 6) |
                  (uint32_t)(b3 & 0x3F);
        *ioff = i;
        return true;
    }

    return false;
}

static bool is_speakable_cp(uint32_t cp)
{
    return ((cp >= 0x4E00 && cp <= 0x9FFF) ||
            (cp >= 'A' && cp <= 'Z') ||
            (cp >= 'a' && cp <= 'z') ||
            (cp >= '0' && cp <= '9'));
}

static bool is_punct_cp(uint32_t cp)
{
    return (cp == ',' || cp == '.' || cp == '!' || cp == '?' || cp == ':' || cp == ';' ||
            cp == 0xFF0C || cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F ||
            cp == 0xFF1A || cp == 0xFF1B || cp == 0x3001);
}

static bool is_quote_or_bracket_cp(uint32_t cp)
{
    return (cp == 0x2018 || cp == 0x2019 || cp == 0x201C || cp == 0x201D ||
            cp == 0x300C || cp == 0x300D || cp == 0x300E || cp == 0x300F ||
            cp == 0x3010 || cp == 0x3011 || cp == 0x3008 || cp == 0x3009 ||
            cp == 0x3014 || cp == 0x3015 || cp == 0xFF08 || cp == 0xFF09 ||
            cp == '(' || cp == ')' || cp == '[' || cp == ']' || cp == '{' || cp == '}');
}

static size_t utf8_fragment_len(const char *src, size_t src_len, size_t max_chars)
{
    size_t i = 0;
    size_t chars = 0;
    size_t last_good = 0;

    while (i < src_len && chars < max_chars) {
        uint32_t cp = 0;
        size_t prev = i;
        if (!utf8_decode_next(src, src_len, &i, &cp)) {
            break;
        }

        if (i == prev) {
            break;
        }

        last_good = i;
        chars++;

        if (is_punct_cp(cp) || cp == ' ') {
            break;
        }
    }

    return last_good;
}

static void sanitize_tts_text(const char *src, char *dst, size_t dst_cap)
{
    size_t in_len = strlen(src);
    size_t i = 0;
    size_t out = 0;
    bool has_speakable = false;
    bool has_letter_or_cjk = false;
    int speakable_count = 0;
    uint32_t prev_cp = 0;

    while (i < in_len && out + 4 < dst_cap) {
        uint32_t cp = 0;
        size_t prev = i;
        if (!utf8_decode_next(src, in_len, &i, &cp)) {
            /* Skip invalid byte and resync. */
            i = prev + 1;
            continue;
        }

        /* Normalize risky punctuation to full-width comma. */
        if (cp == 0x3001 || cp == 0xFF1B || cp == 0xFF1A || cp == 0x2014 || cp == 0x2026 ||
            cp == 0xFF0E || cp == 0x2022 || cp == 0x00B7) {
            cp = 0xFF0C;
        }

        /* Replace quotes/brackets with spaces to avoid parser edge cases. */
        if (is_quote_or_bracket_cp(cp)) {
            cp = ' ';
        }

        /* Normalize line breaks/tabs to spaces to avoid parser skip-only chunks. */
        if (cp == '\n' || cp == '\r' || cp == '\t') {
            cp = ' ';
        }

        if (cp == '|' || cp == '/' || cp == '\\' || cp == '<' || cp == '>' || cp == '~') {
            cp = ' ';
        }

        if (cp < 0x20 && cp != '\n' && cp != '\t' && cp != ' ') {
            continue;
        }

        if (is_speakable_cp(cp)) {
            has_speakable = true;
            speakable_count++;
            if (speakable_count > TTS_SAFE_SPEAKABLE_MAX) {
                break;
            }
            if ((cp >= 0x4E00 && cp <= 0x9FFF) ||
                (cp >= 'A' && cp <= 'Z') ||
                (cp >= 'a' && cp <= 'z')) {
                has_letter_or_cjk = true;
            }
        } else if (!is_punct_cp(cp) && cp != ' ') {
            continue;
        }

        /* Collapse punctuation/space runs to keep parser input simple. */
        if ((cp == ' ' && prev_cp == ' ') ||
            (is_punct_cp(cp) && (is_punct_cp(prev_cp) || prev_cp == ' '))) {
            continue;
        }

        size_t n = utf8_encode_cp(dst + out, dst_cap - out - 1, cp);
        if (n == 0) break;
        out += n;
        prev_cp = cp;
    }

    while (out > 0 && dst[out - 1] == ' ') out--;
    size_t lead = 0;
    while (lead < out && dst[lead] == ' ') lead++;
    if (lead > 0 && lead < out) {
        memmove(dst, dst + lead, out - lead);
        out -= lead;
    } else if (lead >= out) {
        out = 0;
    }

    while (out > 0) {
        size_t j = out;
        uint32_t tail = 0;
        size_t p = 0;
        while (p < j) {
            size_t prev = p;
            if (!utf8_decode_next(dst, j, &p, &tail)) {
                p = prev + 1;
                tail = 0;
            }
        }
        if (tail == ' ' || is_punct_cp(tail)) {
            while (out > 0 && ((unsigned char)dst[out - 1] & 0xC0) == 0x80) out--;
            if (out > 0) out--;
            continue;
        }
        break;
    }

    if (!has_speakable || !has_letter_or_cjk) {
        out = 0;
    }

    dst[out] = '\0';
}

static void tts_worker_task(void *arg)
{
    (void)arg;
    tts_work_t work;

    /* Subscribe this task to TWDT so we can reset it around the blocking parse. */
    esp_task_wdt_add(NULL);

    while (1) {
        /* Wait for work with a bounded timeout so we can reset the WDT
         * while idle, preventing spurious TWDT warnings. */
        if (xQueueReceive(s_work_queue, &work, pdMS_TO_TICKS(5000)) != pdTRUE) {
            esp_task_wdt_reset();
            continue;
        }

        if (!s_tts_ready || s_tts == NULL || strlen(work.text) == 0) {
            /* Silently skip invalid work */
            continue;
        }

        s_speaking = true;
        s_stop_requested = false;

        /* Mute first to prevent pop, then unmute to target volume */
        es8388_start_playback();
        es8388_set_volume(0);
        vTaskDelay(pdMS_TO_TICKS(20));
        es8388_set_volume(s_volume_db);

        char safe_text[TTS_SAFE_TEXT_MAX];
        sanitize_tts_text(work.text, safe_text, sizeof(safe_text));
        if (safe_text[0] == '\0') {
            ESP_LOGW(TAG, "skip empty/invalid text after sanitize");
            esp_tts_stream_reset(s_tts);
            es8388_stop_playback();
            s_speaking = false;
            if (work.cb) {
                tts_done_cb_t cb = work.cb;
                work.cb = NULL;
                cb();
            }
            continue;
        }

        s_utt_count++;
        if (tts_recreate_if_needed() != ESP_OK) {
            ESP_LOGW(TAG, "skip utterance because TTS recreate failed");
            es8388_stop_playback();
            s_speaking = false;
            if (work.cb) {
                tts_done_cb_t cb = work.cb;
                work.cb = NULL;
                cb();
            }
            continue;
        }

        /* Yield so idle task can run before the blocking parse. */
        vTaskDelay(pdMS_TO_TICKS(5));

        /* Temporarily remove this task from TWDT monitoring: esp_tts_parse_chinese
         * is a blocking CPU-bound call whose duration is unbounded for some inputs.
         * Re-register immediately after it returns. */
        esp_task_wdt_delete(NULL);

        size_t safe_len = strlen(safe_text);
        size_t offset = 0;
        int play_speed = s_speed;
        if (play_speed < 0) play_speed = 0;
        if (play_speed > 5) play_speed = 5;

        ESP_LOGI(TAG, "Parsing: %s", safe_text);

        while (offset < safe_len && !s_stop_requested) {
            size_t frag_len = utf8_fragment_len(safe_text + offset, safe_len - offset, TTS_PARSE_FRAGMENT_CHARS);
            if (frag_len == 0) {
                break;
            }

            char parse_text[24];
            size_t copy_len = frag_len;
            if (copy_len >= sizeof(parse_text)) {
                copy_len = sizeof(parse_text) - 1;
                while (copy_len > 0 && ((unsigned char)safe_text[offset + copy_len] & 0xC0) == 0x80) {
                    copy_len--;
                }
            }

            memcpy(parse_text, safe_text + offset, copy_len);
            parse_text[copy_len] = '\0';

            if (parse_text[0] == '\0') {
                offset += frag_len;
                continue;
            }

            esp_tts_stream_reset(s_tts);
            esp_tts_parse_chinese(s_tts, parse_text);
            vTaskDelay(pdMS_TO_TICKS(2));

            int len;
            short *data;

            while (!s_stop_requested) {
                data = esp_tts_stream_play(s_tts, &len, play_speed);
                if (len <= 0) break;

                uint8_t *pcm = (uint8_t *)data;
                int remaining_bytes = len;
                while (remaining_bytes > 0 && !s_stop_requested) {
                    int chunk = remaining_bytes;
                    if (chunk > 1024) chunk = 1024;
                    if (chunk & 0x01) chunk -= 1;
                    if (chunk <= 0) break;

                    if (s_tx_handle) {
                        esp_err_t i2s_ret = i2s_channel_write(
                            s_tx_handle, pcm, (size_t)chunk, NULL, pdMS_TO_TICKS(1000));
                        if (i2s_ret != ESP_OK) {
                            ESP_LOGW(TAG, "I2S write timeout/err, abort sentence");
                            s_stop_requested = true;
                            break;
                        }
                    }
                    pcm += chunk;
                    remaining_bytes -= chunk;
                }
            }

            if (s_tx_handle && !s_stop_requested) {
                int16_t silence[16];
                memset(silence, 0, sizeof(silence));
                esp_err_t i2s_ret = i2s_channel_write(
                    s_tx_handle, silence, sizeof(silence), NULL, pdMS_TO_TICKS(500));
                if (i2s_ret != ESP_OK) {
                    ESP_LOGW(TAG, "silence write timeout");
                }
            }

            offset += frag_len;
            if (offset < safe_len && safe_text[offset] == ' ') {
                offset++;
            }

            esp_tts_stream_reset(s_tts);
        }

        /* Re-subscribe once per utterance after parse/playback completes. */
        esp_task_wdt_add(NULL);
        esp_task_wdt_reset();

        /* Must reset between sentences — without it the engine
         * accumulates PCM data and triggers task WDT (>5s). */
        esp_tts_stream_reset(s_tts);
        es8388_stop_playback();
        s_speaking = false;
        ESP_LOGI(TAG, "Playback done");

        /* Fire completion callback (runs in worker task context) */
        if (work.cb) {
            tts_done_cb_t cb = work.cb;
            work.cb = NULL;
            cb();
        }
    }
}

/* ── Stop helper ─────────────────────────────────────────────────── */

static void tts_stop_internal(void)
{
    s_stop_requested = true;

    /* Purge any queued but not-yet-processed work */
    if (s_work_queue) {
        xQueueReset(s_work_queue);
    }

    int waited_ms = 0;
    while (s_speaking && waited_ms < TTS_STOP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(TTS_STOP_POLL_MS));
        waited_ms += TTS_STOP_POLL_MS;
    }

    if (s_speaking) {
        ESP_LOGW(TAG, "Stop timed out (%dms)", TTS_STOP_WAIT_MS);
        s_speaking = false;
    }
    es8388_stop_playback();
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t app_tts_init(void)
{
    /* Init I2S TX channel */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 256,
        .auto_clear = true,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL), TAG, "i2s new");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TTS_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S0_MCLK,
            .bclk = PIN_I2S0_BCLK,
            .ws   = PIN_I2S0_LRCK,
            .dout = PIN_I2S0_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {false, false, false},
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "i2s init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "i2s enable");

    /* Init ES8388 audio codec */
    ESP_RETURN_ON_ERROR(es8388_init(), TAG, "es8388 init");

    /* Start with minimum volume to prevent pop noise on speaker enable */
    es8388_set_volume(0);
    ESP_RETURN_ON_ERROR(xl9555_speaker_enable(true), TAG, "speaker enable");
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Init esp_tts Chinese TTS engine (xiaole voice) */
    s_voice = esp_tts_voice_set_init(
        &esp_tts_voice_xiaole,
        (void *)_binary_esp_tts_voice_data_xiaole_dat_start);
    if (s_voice == NULL) {
        ESP_LOGE(TAG, "TTS voice init failed");
        s_tts_ready = false;
        return ESP_FAIL;
    }

    s_tts = esp_tts_create(s_voice);
    if (s_tts == NULL) {
        ESP_LOGE(TAG, "TTS create failed");
        esp_tts_voice_set_free(s_voice);
        s_voice = NULL;
        s_tts_ready = false;
        return ESP_FAIL;
    }

    s_tts_ready = true;

    /* Create work queue and persistent worker task */
    s_work_queue = xQueueCreate(4, sizeof(tts_work_t));
    if (!s_work_queue) {
        ESP_LOGE(TAG, "work queue create failed");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(tts_worker_task, "tts_work", TTS_WORKER_STACK, NULL, TTS_WORKER_PRIO, &s_worker_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "tts worker create failed");
        vQueueDelete(s_work_queue);
        s_work_queue = NULL;
        if (s_tts) {
            esp_tts_destroy(s_tts);
            s_tts = NULL;
        }
        if (s_voice) {
            esp_tts_voice_set_free(s_voice);
            s_voice = NULL;
        }
        s_tts_ready = false;
        (void)xl9555_speaker_enable(false);
        if (s_tx_handle) {
            (void)i2s_channel_disable(s_tx_handle);
            (void)i2s_del_channel(s_tx_handle);
            s_tx_handle = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TTS ready (sample rate=%d, voice=xiaole)", TTS_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t app_tts_speak(const char *text)
{
    if (!text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;
    if (!s_tts_ready || s_tts == NULL) return ESP_ERR_INVALID_STATE;
    if (!s_work_queue) return ESP_ERR_INVALID_STATE;

    /* Stop any currently playing speech */
    if (s_speaking) {
        tts_stop_internal();
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    tts_work_t work;
    size_t tlen = strlen(text);
    if (tlen > sizeof(work.text) - 1) tlen = sizeof(work.text) - 1;
    memcpy(work.text, text, tlen);
    work.text[tlen] = '\0';
    work.cb = NULL;

    if (xQueueSend(s_work_queue, &work, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "work queue full, dropping: %s", text);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t app_tts_speak_cb(const char *text, tts_done_cb_t cb)
{
    if (!text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;
    if (!s_tts_ready || s_tts == NULL) return ESP_ERR_INVALID_STATE;
    if (!s_work_queue) return ESP_ERR_INVALID_STATE;

    if (s_speaking) {
        tts_stop_internal();
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    tts_work_t work;
    size_t tlen = strlen(text);
    if (tlen > sizeof(work.text) - 1) tlen = sizeof(work.text) - 1;
    memcpy(work.text, text, tlen);
    work.text[tlen] = '\0';
    work.cb = cb;

    if (xQueueSend(s_work_queue, &work, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "work queue full, dropping: %s", text);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void app_tts_stop(void)
{
    tts_stop_internal();
}

bool app_tts_is_busy(void)
{
    return s_speaking;
}

/* ── Volume control ──────────────────────────────────────────────── */

esp_err_t app_tts_set_volume(int vol_db)
{
    if (vol_db < -48) vol_db = -48;
    if (vol_db > 0)    vol_db = 0;
    s_volume_db = vol_db;
    /* If currently playing, apply immediately */
    if (s_speaking) {
        es8388_set_volume(s_volume_db);
    }
    ESP_LOGI(TAG, "Volume set to %d dB", s_volume_db);
    return ESP_OK;
}

int app_tts_get_volume(void)
{
    return s_volume_db;
}

/* ── Speed control ──────────────────────────────────────────────── */

esp_err_t app_tts_set_speed(int speed)
{
    if (speed < 0) speed = 0;
    if (speed > 5) speed = 5;
    s_speed = speed;
    ESP_LOGI(TAG, "Speed set to %d", s_speed);
    return ESP_OK;
}

int app_tts_get_speed(void)
{
    return s_speed;
}

void app_tts_play_beep(void)
{
    /* Use dedicated buzzer pin (XL9555 BEEP, active-low).
     * 35ms pulse — doesn't touch I2S, no conflict with TTS worker. */
    xl9555_beep_enable(true);
    vTaskDelay(pdMS_TO_TICKS(35));
    xl9555_beep_enable(false);
}
