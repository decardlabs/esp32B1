#include "app_cloud_tts.h"
#include "app_config.h"
#include "app_tts.h"
#include "board_pins.h"
#include "es8388.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"

static const char *TAG = "CLOUD_TTS";

#define RING_BUF_SIZE  (96 * 1024)
#define PREFETCH_TEXT_MAX 512

typedef enum {
    AUDIO_TARGET_RING = 0,
    AUDIO_TARGET_PREFETCH,
} audio_decode_target_t;

static i2s_chan_handle_t s_tx_handle = NULL;
static volatile bool s_cloud_busy = false;
static volatile bool s_stop_requested = false;
static volatile bool s_play_requested = false;
static cloud_tts_done_cb_t s_done_cb = NULL;
static esp_http_client_handle_t s_http_client = NULL;
static audio_decode_target_t s_decode_target = AUDIO_TARGET_RING;

static uint8_t *s_ring = NULL;
static volatile size_t s_ring_w = 0;
static volatile size_t s_ring_r = 0;
static volatile bool s_ring_done = false;
static SemaphoreHandle_t s_ring_mutex = NULL;
static StaticSemaphore_t s_ring_mutex_buffer;

static void ring_reset(void)
{
    s_ring_w = 0;
    s_ring_r = 0;
    s_ring_done = false;
}

static size_t ring_available(void)
{
    if (!s_ring_mutex) return 0;
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    size_t avail = (s_ring_w >= s_ring_r)
        ? (s_ring_w - s_ring_r)
        : (RING_BUF_SIZE - s_ring_r + s_ring_w);
    xSemaphoreGive(s_ring_mutex);
    return avail;
}

static size_t ring_write(const uint8_t *data, size_t len)
{
    if (!s_ring_mutex) return 0;
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    size_t written = 0;
    while (written < len) {
        size_t space = (s_ring_r > s_ring_w)
            ? (s_ring_r - s_ring_w - 1)
            : (RING_BUF_SIZE - s_ring_w + (s_ring_r > 0 ? s_ring_r - 1 : RING_BUF_SIZE - 1));
        if (space == 0) break;
        size_t chunk = len - written;
        size_t contig = RING_BUF_SIZE - s_ring_w;
        if (chunk > contig) chunk = contig;
        if (chunk > space) chunk = space;
        memcpy(s_ring + s_ring_w, data + written, chunk);
        s_ring_w = (s_ring_w + chunk) % RING_BUF_SIZE;
        written += chunk;
    }
    xSemaphoreGive(s_ring_mutex);
    return written;
}

static size_t ring_write_blocking(const uint8_t *data, size_t len)
{
    size_t written = 0;

    while (written < len && !s_stop_requested) {
        size_t n = ring_write(data + written, len - written);
        if (n > 0) {
            written += n;
            continue;
        }

        /* Wait for audio_task to drain the ring instead of dropping PCM. */
        vTaskDelay(pdMS_TO_TICKS(4));
    }

    return written;
}

static size_t ring_read(uint8_t *buf, size_t len)
{
    if (!s_ring_mutex) return 0;
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    size_t read = 0;
    while (read < len && s_ring_r != s_ring_w) {
        size_t contig = RING_BUF_SIZE - s_ring_r;
        size_t chunk = len - read;
        if (chunk > contig) chunk = contig;
        size_t avail = (s_ring_w > s_ring_r)
            ? (s_ring_w - s_ring_r)
            : (RING_BUF_SIZE - s_ring_r);
        if (chunk > avail) chunk = avail;
        if (chunk == 0) break;
        memcpy(buf + read, s_ring + s_ring_r, chunk);
        s_ring_r = (s_ring_r + chunk) % RING_BUF_SIZE;
        read += chunk;
    }
    xSemaphoreGive(s_ring_mutex);
    return read;
}

/* HTTP response body accumulator (JSON may span multiple ON_DATA events) */
static char *s_resp_buf = NULL;
static size_t s_resp_len = 0;
static size_t s_resp_cap = 0;

/* Persistent PCM decode buffer (reused across sentences to reduce fragmentation) */
static uint8_t *s_pcm_buf = NULL;
static size_t s_pcm_cap = 0;

static uint8_t *s_prefetch_pcm = NULL;
static size_t s_prefetch_pcm_len = 0;
static size_t s_prefetch_pcm_cap = 0;
static char s_prefetch_text[PREFETCH_TEXT_MAX] = {0};
static bool s_prefetch_ready = false;

static void *psram_realloc_or_alloc(void *ptr, size_t new_size);
static esp_err_t http_data_cb(esp_http_client_event_t *evt);

static size_t sanitize_tts_text(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;

    if (!src || !dst || dst_size == 0) {
        return 0;
    }

    for (const char *p = src; *p && out < dst_size - 4; p++) {
        if (*p == '"' || *p == '\\') {
            dst[out++] = ' ';
        } else if ((unsigned char)*p < 0x20) {
            dst[out++] = ' ';
        } else {
            dst[out++] = *p;
        }
    }

    dst[out] = '\0';
    return out;
}

static void clear_prefetch_cache(void)
{
    s_prefetch_pcm_len = 0;
    s_prefetch_ready = false;
    s_prefetch_text[0] = '\0';
}

static void reset_http_accumulator(void)
{
    s_resp_buf = (char *)psram_realloc_or_alloc(s_resp_buf, 0);
    s_resp_buf = NULL;
    s_resp_len = 0;
    s_resp_cap = 0;
}

static esp_err_t ensure_http_client_ready(void)
{
    if (s_http_client) {
        return ESP_OK;
    }

    const char *url = "https://openspeech.bytedance.com/api/v3/tts/unidirectional";

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_data_cb,
    };

    s_http_client = esp_http_client_init(&http_cfg);
    if (!s_http_client) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTTP keep-alive client initialized");
    return ESP_OK;
}

static void rebuild_http_client(void)
{
    if (s_http_client) {
        esp_http_client_cleanup(s_http_client);
        s_http_client = NULL;
    }
    reset_http_accumulator();
}

static void *psram_realloc_or_alloc(void *ptr, size_t new_size)
{
    if (new_size == 0) {
        if (ptr) {
            heap_caps_free(ptr);
        }
        return NULL;
    }

    if (ptr == NULL) {
        return heap_caps_malloc(new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* Persistent audio task */
static TaskHandle_t s_audio_task = NULL;
#define CLOUD_AUDIO_STACK 8192
static StackType_t s_audio_stack[CLOUD_AUDIO_STACK];
static StaticTask_t s_audio_tcb;

/* Decode one base64 audio field and append it to the playback ring. */
static size_t decode_audio_field_to_ring(const char *audio_start, size_t b64_len)
{
    size_t max_pcm = (b64_len * 3 / 4) + 4;

    /* Use persistent PCM buffer to avoid per-sentence malloc/free fragmentation */
    if (max_pcm > s_pcm_cap) {
        uint8_t *newbuf = (uint8_t *)psram_realloc_or_alloc(s_pcm_buf, max_pcm);
        if (!newbuf) {
            ESP_LOGW(TAG, "PCM buf realloc(%u) failed", (unsigned)max_pcm);
            return 0;
        }
        s_pcm_buf = newbuf;
        s_pcm_cap = max_pcm;
    }

    size_t pcm_len = 0;
    int ret = mbedtls_base64_decode(s_pcm_buf, s_pcm_cap, &pcm_len,
                 (const unsigned char *)audio_start, b64_len);
    if (ret == 0 && pcm_len > 0) {
        if (s_decode_target == AUDIO_TARGET_PREFETCH) {
            size_t need = s_prefetch_pcm_len + pcm_len;
            if (need > s_prefetch_pcm_cap) {
                size_t new_cap = s_prefetch_pcm_cap ? s_prefetch_pcm_cap : 8192;
                while (new_cap < need) {
                    new_cap *= 2;
                }
                uint8_t *newbuf = (uint8_t *)psram_realloc_or_alloc(s_prefetch_pcm, new_cap);
                if (!newbuf) {
                    ESP_LOGW(TAG, "prefetch realloc(%u) failed", (unsigned)new_cap);
                    return 0;
                }
                s_prefetch_pcm = newbuf;
                s_prefetch_pcm_cap = new_cap;
            }

            memcpy(s_prefetch_pcm + s_prefetch_pcm_len, s_pcm_buf, pcm_len);
            s_prefetch_pcm_len += pcm_len;
            ESP_LOGI(TAG, "prefetch chunk: b64=%u pcm=%u total=%u",
                     (unsigned)b64_len, (unsigned)pcm_len, (unsigned)s_prefetch_pcm_len);
            return pcm_len;
        } else {
            size_t written = ring_write_blocking(s_pcm_buf, pcm_len);
            ESP_LOGI(TAG, "audio chunk: b64=%u pcm=%u ring_write=%u",
                     (unsigned)b64_len, (unsigned)pcm_len, (unsigned)written);
            return written;
        }
    } else {
        ESP_LOGW(TAG, "process_audio: base64_decode ret=%d b64_len=%u max_pcm=%u",
                 ret, (unsigned)b64_len, (unsigned)max_pcm);
        return 0;
    }
}

/* Extract all base64 audio fragments from the response and write them to ring. */
static void process_audio_payload(const char *payload)
{
    const char *cursor = payload;
    size_t total_written = 0;
    int fragments = 0;

    while (cursor && *cursor) {
        const char *audio_start = strstr(cursor, "\"data\":\"");
        if (!audio_start) {
            break;
        }
        audio_start += 8;

        const char *audio_end = strchr(audio_start, '"');
        if (!audio_end || audio_end <= audio_start) {
            break;
        }

        size_t b64_len = (size_t)(audio_end - audio_start);
        if (b64_len > 0) {
            total_written += decode_audio_field_to_ring(audio_start, b64_len);
            fragments++;
        }

        cursor = audio_end + 1;
    }

    if (s_decode_target == AUDIO_TARGET_PREFETCH) {
        ESP_LOGI(TAG, "prefetch fragments=%d total_prefetch_written=%u resp_bytes=%u",
                 fragments, (unsigned)total_written, (unsigned)s_resp_len);
    } else {
        ESP_LOGI(TAG, "audio fragments=%d total_ring_written=%u resp_bytes=%u",
                 fragments, (unsigned)total_written, (unsigned)s_resp_len);
    }
}

/* HTTP streaming event callback — parses SSE data: lines as they arrive */
/* HTTP streaming event callback — accumulates JSON response body,
 * then extracts base64 audio data on finish. */
static esp_err_t http_data_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_stop_requested) return ESP_FAIL;
        /* Accumulate response body (may span multiple ON_DATA events). Keep it in PSRAM. */
        size_t needed = s_resp_len + evt->data_len + 1;
        if (needed > s_resp_cap) {
            size_t new_cap = s_resp_cap ? s_resp_cap : 4096;
            while (new_cap < needed) {
                new_cap *= 2;
            }
            char *newbuf = (char *)psram_realloc_or_alloc(s_resp_buf, new_cap);
            if (!newbuf) return ESP_ERR_NO_MEM;
            s_resp_buf = newbuf;
            s_resp_cap = new_cap;
        }
        memcpy(s_resp_buf + s_resp_len, evt->data, evt->data_len);
        s_resp_len += evt->data_len;
    }

    if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        ESP_LOGI(TAG, "HTTP status=%d, resp=%u bytes",
                 esp_http_client_get_status_code(evt->client), (unsigned)s_resp_len);
        /* Parse full JSON response: {"code":0,"data":"<base64>"} */
        if (s_resp_buf && s_resp_len > 0) {
            s_resp_buf[s_resp_len] = '\0';
            process_audio_payload(s_resp_buf);
        }
        s_resp_buf = (char *)psram_realloc_or_alloc(s_resp_buf, 0);
        s_resp_buf = NULL;
        s_resp_len = 0;
        s_resp_cap = 0;
    }

    return ESP_OK;
}

/* ── Persistent audio playback task ────────────────────────────────── */
/* Created once in app_cloud_tts_init(). Loops forever; waits for
 * s_play_requested, reads PCM from ring buffer → I2S → ES8388. */

static void audio_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];

    while (1) {
        /* Wait for a play request */
        if (!s_play_requested) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Wait for initial data (SSE callback writes to ring) or stop */
        {
            uint32_t wait_loops = 0;
            while (!s_stop_requested && ring_available() == 0 && !s_ring_done) {
                vTaskDelay(pdMS_TO_TICKS(10));
                wait_loops++;
                if (wait_loops % 100 == 0) {
                    ESP_LOGD(TAG, "audio_task waiting for data...");
                }
            }
            ESP_LOGI(TAG, "audio_task: after wait ring_avail=%u s_ring_done=%d s_stop=%d",
                     (unsigned)ring_available(), (int)s_ring_done, (int)s_stop_requested);
        }
        if (s_stop_requested) {
            s_play_requested = false;
            continue;
        }
        if (s_ring_done && ring_available() == 0) {
            ESP_LOGW(TAG, "audio_task: no data received");
            s_play_requested = false;
            s_cloud_busy = false;
            if (s_done_cb) {
                cloud_tts_done_cb_t cb = s_done_cb;
                s_done_cb = NULL;
                cb();
            }
            continue;
        }

        ESP_LOGI(TAG, "audio_task: starting playback");
        s_cloud_busy = true;
        es8388_start_playback();
        es8388_set_volume(-12);
        /* Allow ES8388 PLL to stabilize after MCLK resume */
        vTaskDelay(pdMS_TO_TICKS(50));

        {
            TickType_t next_stack_log_tick = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
            while (!s_stop_requested) {
                TickType_t now = xTaskGetTickCount();
                if ((int32_t)(now - next_stack_log_tick) >= 0) {
                    UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
                    ESP_LOGI(TAG, "[STACK] task=audio watermark=%u", (unsigned)wm);
                    next_stack_log_tick = now + pdMS_TO_TICKS(5000);
                }
                size_t n = ring_read(buf, sizeof(buf));
                if (n > 0) {
                    if (s_tx_handle) {
                        esp_err_t i2s_ret = i2s_channel_write(s_tx_handle, buf, n, NULL,
                                                               pdMS_TO_TICKS(1000));
                        if (i2s_ret != ESP_OK) {
                            ESP_LOGW(TAG, "I2S write failed (%s), stopping",
                                     esp_err_to_name(i2s_ret));
                            break;
                        }
                    }
                } else if (s_ring_done) {
                    break;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
        }

        /* Drain remaining PCM from ring buffer */
        {
            uint8_t drain[256];
            size_t dn;
            do {
                dn = ring_read(drain, sizeof(drain));
                if (dn > 0 && s_tx_handle)
                    i2s_channel_write(s_tx_handle, drain, dn, NULL, pdMS_TO_TICKS(1000));
            } while (dn > 0);
        }

        es8388_stop_playback();
        s_play_requested = false;
        s_cloud_busy = false;

        if (s_done_cb) {
            cloud_tts_done_cb_t cb = s_done_cb;
            s_done_cb = NULL;
            cb();
        }
        ESP_LOGI(TAG, "audio_task: playback finished");
    }
}

/* Stream TTS request — X-Api-Key auth (new console), SSE processed in http_data_cb */
static esp_err_t send_tts_request(const char *text, const char *api_key,
                                   const char *resource_id, const char *voice,
                                   bool prefetch_mode)
{
    char reqid[64];
    snprintf(reqid, sizeof(reqid), "esp_%u", (unsigned)(xTaskGetTickCount()));

    /* Sanitize voice parameter to prevent JSON injection (§6.2) */
    char safe_voice[64];
    size_t sv = 0;
    if (voice) {
        for (const char *p = voice; *p && sv < sizeof(safe_voice) - 4; p++) {
            if (*p == '"' || *p == '\\')
                safe_voice[sv++] = ' ';
            else if ((unsigned char)*p < 0x20)
                safe_voice[sv++] = ' ';
            else
                safe_voice[sv++] = *p;
        }
    }
    safe_voice[sv] = '\0';

    char body[1536];
    snprintf(body, sizeof(body),
        "{"
        "\"user\":{\"uid\":\"esp32s3_001\"},"
        "\"namespace\":\"BidirectionalTTS\","
        "\"req_params\":{"
        "\"text\":\"%s\","
        "\"speaker\":\"%s\","
        "\"model\":\"seed-tts-2.0-standard\","
        "\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":16000}"
        "},"
        "\"request_id\":\"%s\""
        "}",
        text, safe_voice, reqid);

    ESP_LOGI(TAG, "v3 req body: %s", body);

    if (ensure_http_client_ready() != ESP_OK) {
        return ESP_FAIL;
    }

    reset_http_accumulator();

    esp_http_client_set_method(s_http_client, HTTP_METHOD_POST);
    esp_http_client_set_header(s_http_client, "Connection", "keep-alive");
    esp_http_client_set_header(s_http_client, "Content-Type", "application/json");
    esp_http_client_set_header(s_http_client, "Accept", "text/event-stream, application/json");
    ESP_LOGI(TAG, "auth: X-Api-Key");
    esp_http_client_set_header(s_http_client, "X-Api-Key", api_key);
    esp_http_client_set_header(s_http_client, "X-Api-Resource-Id", resource_id);

    esp_http_client_set_post_field(s_http_client, body, strlen(body));
    s_decode_target = prefetch_mode ? AUDIO_TARGET_PREFETCH : AUDIO_TARGET_RING;
    if (prefetch_mode) {
        s_prefetch_pcm_len = 0;
    }

    esp_err_t err = esp_http_client_perform(s_http_client);
    int status = esp_http_client_get_status_code(s_http_client);
    if (!prefetch_mode) {
        s_ring_done = true;
    }

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP %d err=%s", status, esp_err_to_name(err));
        if (prefetch_mode) {
            s_prefetch_pcm_len = 0;
        }
        rebuild_http_client();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "v3 OK (status=%d), streaming complete", status);
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t app_cloud_tts_init(void)
{
    if (s_ring) return ESP_OK;
    s_ring = (uint8_t *)heap_caps_malloc(RING_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) return ESP_ERR_NO_MEM;
    memset(s_ring, 0, RING_BUF_SIZE);
    s_ring_mutex = xSemaphoreCreateMutexStatic(&s_ring_mutex_buffer);
    if (!s_ring_mutex) return ESP_ERR_NO_MEM;

    /* Create persistent audio task */
    s_audio_task = xTaskCreateStatic(audio_task, "cloud_audio", CLOUD_AUDIO_STACK,
                                      NULL, 5, s_audio_stack, &s_audio_tcb);
    if (!s_audio_task) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Cloud TTS init done (96KB ring, persistent audio task)");
    return ESP_OK;
}

esp_err_t app_cloud_tts_speak(const char *text, cloud_tts_done_cb_t cb)
{
    if (!text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;

    if (s_cloud_busy) {
        app_cloud_tts_stop();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    char safe_text[512];
    size_t out = sanitize_tts_text(text, safe_text, sizeof(safe_text));

    if (out == 0) {
        ESP_LOGW(TAG, "Empty text after sanitization");
        s_cloud_busy = false;
        if (cb) cb();
        return ESP_ERR_INVALID_ARG;
    }

    s_tx_handle = (i2s_chan_handle_t)app_tts_get_i2s_handle();
    if (!s_tx_handle) {
        ESP_LOGE(TAG, "I2S handle not available");
        s_cloud_busy = false;
        if (s_done_cb) {
            cloud_tts_done_cb_t cb = s_done_cb;
            s_done_cb = NULL;
            cb();
        }
        return ESP_ERR_INVALID_STATE;
    }

    s_done_cb = cb;

    if (s_prefetch_ready && strcmp(s_prefetch_text, safe_text) == 0 && s_prefetch_pcm_len > 0) {
        ESP_LOGI(TAG, "prefetch hit: using cached audio (%u bytes)", (unsigned)s_prefetch_pcm_len);

        ring_reset();
        s_stop_requested = false;
        s_cloud_busy = true;
        s_play_requested = true;

        size_t written = ring_write_blocking(s_prefetch_pcm, s_prefetch_pcm_len);
        s_ring_done = true;
        if (written < s_prefetch_pcm_len) {
            ESP_LOGW(TAG, "prefetch playback partial write: %u/%u",
                     (unsigned)written, (unsigned)s_prefetch_pcm_len);
        }
        clear_prefetch_cache();
        return ESP_OK;
    }

    /* Reset playback state before signaling audio_task to avoid stale
     * s_ring_done=1 causing false "no data received" races. */
    ring_reset();
    s_stop_requested = false;

    s_cloud_busy = true;
    s_play_requested = true;  /* Signal persistent audio task to start */

    const char *api_key = g_app_config.tts_api_key[0] ? g_app_config.tts_api_key : NULL;
    const char *res_id = g_app_config.tts_resource_id[0] ? g_app_config.tts_resource_id : "seed-tts-2.0";

    esp_err_t http_err = send_tts_request(safe_text,
                         api_key,
                         res_id,
                         g_app_config.tts_voice,
                         false);

    if (http_err != ESP_OK) {
        ESP_LOGW(TAG, "cloud TTS failed, caller will fallback");
        s_cloud_busy = false;
        s_play_requested = false;
        return http_err;
    }

    return ESP_OK;
}

esp_err_t app_cloud_tts_prefetch(const char *text)
{
    if (!text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;

    char safe_text[512];
    size_t out = sanitize_tts_text(text, safe_text, sizeof(safe_text));
    if (out == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_prefetch_ready && strcmp(s_prefetch_text, safe_text) == 0 && s_prefetch_pcm_len > 0) {
        return ESP_OK;
    }

    const char *api_key = g_app_config.tts_api_key[0] ? g_app_config.tts_api_key : NULL;
    const char *res_id = g_app_config.tts_resource_id[0] ? g_app_config.tts_resource_id : "seed-tts-2.0";

    strncpy(s_prefetch_text, safe_text, sizeof(s_prefetch_text) - 1);
    s_prefetch_text[sizeof(s_prefetch_text) - 1] = '\0';
    s_prefetch_ready = false;
    s_prefetch_pcm_len = 0;

    esp_err_t http_err = send_tts_request(safe_text,
                                          api_key,
                                          res_id,
                                          g_app_config.tts_voice,
                                          true);
    if (http_err != ESP_OK) {
        clear_prefetch_cache();
        return http_err;
    }

    if (s_prefetch_pcm_len == 0) {
        clear_prefetch_cache();
        return ESP_FAIL;
    }

    s_prefetch_ready = true;
    ESP_LOGI(TAG, "prefetch ready: %u bytes", (unsigned)s_prefetch_pcm_len);
    return ESP_OK;
}

void app_cloud_tts_stop(void)
{
    s_stop_requested = true;
    s_ring_done = true;
    s_cloud_busy = false;
    s_play_requested = false;
    clear_prefetch_cache();
    es8388_stop_playback();
}

bool app_cloud_tts_is_busy(void)
{
    return s_cloud_busy;
}
