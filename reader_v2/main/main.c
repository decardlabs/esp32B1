#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "i2c_bus.h"
#include "bsp_spi.h"
#include "board_pins.h"
#include "sd_card.h"
#include "tusb_msc.h"
#include "es8388.h"
#include "app_buttons.h"
#include "app_tts.h"
#include "app_display.h"
#include "app_config.h"
#include "app_wifi.h"
#include "app_cloud_tts.h"
#include "xl9555.h"
#include "app_oom.h"

#include "esp_heap_caps.h"

#define ENABLE_HEAP_INTEGRITY_SCAN 0

static const char *TAG = "MAIN";

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default: return "UNKNOWN";
    }
}

/* ── File management ─────────────────────────────────────────────────── */

#define MAX_FILES       64
#define MAX_FNAME_LEN   64
#define MAX_FILES_CONTENT (64 * 1024)  /* Upgrade from 4KB to 64KB (Phase 1: crash fix) */
#define MAX_DISPLAY_TEXT 2048

static char (*s_file_names)[MAX_FNAME_LEN] = NULL;
static int s_file_count = 0;
static int s_current_file = -1;
static char *s_file_content = NULL;
static uint8_t *s_file_raw = NULL;
static bool s_reading_buffers_ready = false;
static char s_display_buf[MAX_DISPLAY_TEXT] = {0};
static size_t s_content_len = 0;
static bool s_file_loaded = false;

/* Sentence boundaries */
#define MAX_SENTENCES 256
#define TTS_RUNTIME_MAX_BYTES 72
#define CLOUD_BATCH_MAX_BYTES_NORMAL 220
#define CLOUD_BATCH_MAX_BYTES_LOWMEM 140
#define CLOUD_BATCH_MAX_BYTES_CRITICAL 100
#define CLOUD_BATCH_MAX_SENTENCES_NORMAL 4
#define CLOUD_BATCH_MAX_SENTENCES_LOWMEM 2
#define CLOUD_BATCH_MAX_SENTENCES_CRITICAL 1
static size_t s_sentence_starts[MAX_SENTENCES];
static int s_num_sentences = 0;
static int s_current_sentence = 0;
static size_t s_sentence_offset = 0;
static size_t s_last_chunk_len = 0;
static int s_cloud_batch_sentences = 0;

/* Playback state */
static bool s_playing = false;
static bool s_system_ready = false;
static bool s_tts_available = false;
static bool s_card_available = false;

/* Track whether we forcibly disconnected USB for reading mode */
static bool s_usb_disconnected_for_reading = false;

/* Guard against KEY1 rapid toggling between main menu and USB mode. */
#define KEY1_MODE_SWITCH_GUARD_MS 250
static TickType_t s_last_key1_mode_switch_tick = 0;

/* ── Static FreeRTOS objects (§1.1) ─────────────────────────────────── */

/* Mutex protecting playback state (s_current_sentence, s_sentence_offset, s_playing, etc.) */
static SemaphoreHandle_t s_state_mutex = NULL;
static StaticSemaphore_t s_state_mutex_buffer;

/* Button event queue */
#define BTN_EVENT_QUEUE_LEN 16
typedef struct {
    int key_id;
    btn_event_t event;
} button_msg_t;
static QueueHandle_t s_button_event_queue = NULL;
static StaticQueue_t s_btn_queue_buf;
static uint8_t s_btn_queue_storage[BTN_EVENT_QUEUE_LEN * sizeof(button_msg_t)];

/* Static task buffers */
#define POLL_TASK_STACK 4096
#define BTN_EVT_TASK_STACK 8192
static StackType_t s_poll_stack[POLL_TASK_STACK];
static StaticTask_t s_poll_tcb;
static StackType_t s_btn_evt_stack[BTN_EVT_TASK_STACK];
static StaticTask_t s_btn_evt_tcb;

/* ── State mutex helpers (§4.1) ─────────────────────────────────────── */
/* Protects: s_playing, s_current_sentence, s_sentence_offset,
 *           s_last_chunk_len across tasks (btn_evt, poll, TTS worker). */
#define STATE_LOCK()   xSemaphoreTake(s_state_mutex, portMAX_DELAY)
#define STATE_UNLOCK() xSemaphoreGive(s_state_mutex)

/* Global config loaded from SD card */
app_config_t g_app_config;

/* Operation mode (only one active at a time to avoid FATFS + MSC conflict) */
typedef enum {
    MODE_MAIN_MENU = 0,   /* Waiting for user choice */
    MODE_USB_COPY,         /* USB MSC active, NO local FS access */
    MODE_READING,          /* Local FS access, NO USB */
} op_mode_t;
static op_mode_t s_op_mode = MODE_MAIN_MENU;
static bool s_usb_started = false;
static sdmmc_card_t *s_card = NULL;
static char s_boot_reason[32] = "";
static char s_init_log[512] = "";
/* Debug helper: bypass real USB MSC init so logs stay on monitor.
 * Default ON for crash investigation; KEY4 in main menu can toggle it. */
static bool s_debug_usb_bypass = false;

static void init_step(const char *label, esp_err_t result);

static bool key1_mode_switch_allowed(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t guard_ticks = pdMS_TO_TICKS(KEY1_MODE_SWITCH_GUARD_MS);
    if (guard_ticks == 0) {
        guard_ticks = 1;
    }

    if ((now - s_last_key1_mode_switch_tick) < guard_ticks) {
        return false;
    }

    s_last_key1_mode_switch_tick = now;
    return true;
}

static void mem_snapshot(const char *stage)
{
    size_t free_all = esp_get_free_heap_size();
    size_t min_all = esp_get_minimum_free_heap_size();
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ps_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    /* Fragmentation trend detection (spec §7.2 / Phase 4):
     * consecutive int_largest shrinkage → fragmentation warning. */
    {
        static size_t s_prev_int_largest = 0;
        static int s_shrink_count = 0;
        if (int_largest < s_prev_int_largest && s_prev_int_largest != 0) {
            s_shrink_count++;
            if (s_shrink_count >= 3) {
                ESP_LOGW(TAG, "[MEM] fragmentation warning: int_largest shrinking (%d consecutive)",
                         s_shrink_count);
                s_shrink_count = 0;  /* one alert per episode */
            }
        } else {
            s_shrink_count = 0;
        }
        s_prev_int_largest = int_largest;
    }

    ESP_LOGI(TAG,
             "[MEM] stage=%s free=%u min=%u int_free=%u int_largest=%u ps_free=%u ps_largest=%u",
             stage ? stage : "unknown",
             (unsigned)free_all,
             (unsigned)min_all,
             (unsigned)int_free,
             (unsigned)int_largest,
             (unsigned)ps_free,
             (unsigned)ps_largest);

    app_oom_check(int_free, int_largest);
}

/* ── Text processing ─────────────────────────────────────────────────── */

/* Copy up to (dst_size-1) bytes from src, but never split a UTF-8 multi-byte
 * sequence. Returns the number of bytes copied (excluding the NUL). */
static size_t utf8_safe_copy(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    size_t max = (src_len < dst_size - 1) ? src_len : dst_size - 1;

    /* If we are truncating in the middle of a UTF-8 sequence, back off to the
     * last complete character boundary. Continuation bytes are 0b10xxxxxx. */
    if (max < src_len) {
        while (max > 0 && ((unsigned char)src[max] & 0xC0) == 0x80) {
            max--;
        }
    }

    memcpy(dst, src, max);
    dst[max] = '\0';
    return max;
}

static bool utf8_append_cp(char *dst, size_t dst_size, size_t *out_len, uint32_t cp)
{
    if (cp <= 0x7F) {
        if (*out_len + 1 >= dst_size) return false;
        dst[(*out_len)++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (*out_len + 2 >= dst_size) return false;
        dst[(*out_len)++] = (char)(0xC0 | ((cp >> 6) & 0x1F));
        dst[(*out_len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        if (*out_len + 3 >= dst_size) return false;
        dst[(*out_len)++] = (char)(0xE0 | ((cp >> 12) & 0x0F));
        dst[(*out_len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*out_len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        if (*out_len + 4 >= dst_size) return false;
        dst[(*out_len)++] = (char)(0xF0 | ((cp >> 18) & 0x07));
        dst[(*out_len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        dst[(*out_len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[(*out_len)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*out_len + 1 >= dst_size) return false;
        dst[(*out_len)++] = '?';
    }
    return true;
}

static size_t decode_utf16_to_utf8(const uint8_t *src, size_t src_len, bool little_endian,
                                   char *dst, size_t dst_size)
{
    size_t out_len = 0;
    size_t i = 0;

    while (i + 1 < src_len) {
        uint16_t w1 = little_endian ? (uint16_t)(src[i] | (src[i + 1] << 8))
                                    : (uint16_t)((src[i] << 8) | src[i + 1]);
        i += 2;

        if (w1 == 0x0000) continue;

        uint32_t cp = w1;
        if (w1 >= 0xD800 && w1 <= 0xDBFF && i + 1 < src_len) {
            uint16_t w2 = little_endian ? (uint16_t)(src[i] | (src[i + 1] << 8))
                                        : (uint16_t)((src[i] << 8) | src[i + 1]);
            if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                cp = 0x10000 + (((uint32_t)(w1 - 0xD800) << 10) | (uint32_t)(w2 - 0xDC00));
                i += 2;
            }
        }

        if (!utf8_append_cp(dst, dst_size, &out_len, cp)) {
            break;
        }
    }

    dst[out_len] = '\0';
    return out_len;
}

static size_t normalize_text_to_utf8(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
{
    if (src_len >= 3 && src[0] == 0xEF && src[1] == 0xBB && src[2] == 0xBF) {
        size_t copy = src_len - 3;
        if (copy > dst_size - 1) copy = dst_size - 1;
        memcpy(dst, src + 3, copy);
        dst[copy] = '\0';
        return copy;
    }

    if (src_len >= 2 && src[0] == 0xFF && src[1] == 0xFE) {
        return decode_utf16_to_utf8(src + 2, src_len - 2, true, dst, dst_size);
    }

    if (src_len >= 2 && src[0] == 0xFE && src[1] == 0xFF) {
        return decode_utf16_to_utf8(src + 2, src_len - 2, false, dst, dst_size);
    }

    size_t zero_even = 0;
    size_t zero_odd = 0;
    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == 0) {
            if ((i & 1) == 0) zero_even++;
            else zero_odd++;
        }
    }

    if (src_len >= 8 && (zero_even > src_len / 6 || zero_odd > src_len / 6)) {
        bool little_endian = (zero_odd > zero_even);
        return decode_utf16_to_utf8(src, src_len, little_endian, dst, dst_size);
    }

    size_t copy = src_len;
    if (copy > dst_size - 1) copy = dst_size - 1;
    memcpy(dst, src, copy);
    dst[copy] = '\0';
    return copy;
}

static int split_sentences(const char *text, size_t *starts, int max_s)
{
    const size_t max_chunk_bytes = TTS_RUNTIME_MAX_BYTES;
    int count = 0;
    size_t len = strlen(text);
    size_t last_cut = 0;

    if (len > 0) starts[count++] = 0;

    for (size_t i = 0; i < len && count < max_s; i++) {
        bool should_cut = false;
        unsigned char c = (unsigned char)text[i];

        if (c == '.' || c == '!' || c == '?' || c == '\n' || c == ',') {
            should_cut = true;
        } else if (i + 2 < len && c == 0xE3 && (unsigned char)text[i + 1] == 0x80) {
            unsigned char b2 = (unsigned char)text[i + 2];
            if (b2 == 0x82 || b2 == 0x81) {
                i += 2;
                should_cut = true;
            }
        } else if (i + 2 < len && c == 0xEF && (unsigned char)text[i + 1] == 0xBC) {
            unsigned char b2 = (unsigned char)text[i + 2];
            if (b2 == 0x81 || b2 == 0x8C || b2 == 0x9A || b2 == 0x9B || b2 == 0x9F) {
                i += 2;
                should_cut = true;
            }
        }

        if (should_cut && i + 1 > last_cut) {
            starts[count++] = i + 1;
            last_cut = i + 1;
            continue;
        }

        if (i + 1 - last_cut >= max_chunk_bytes) {
            size_t cut = i + 1;
            while (cut > last_cut && ((unsigned char)text[cut] & 0xC0) == 0x80) {
                cut--;
            }
            if (cut == last_cut) continue;
            starts[count++] = cut;
            last_cut = cut;
        }
    }

    if (count > 0 && starts[count - 1] < len && count < max_s) starts[count++] = len;
    else if (count >= max_s) starts[max_s - 1] = len;
    else if (count == 0) { starts[0] = 0; starts[1] = len; count = 2; }
    return count;
}

static size_t tts_pick_chunk_len(const char *text, size_t len, size_t max_bytes)
{
    size_t n = len;
    if (n > max_bytes) n = max_bytes;
    while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
        n--;
    }
    if (n == 0) {
        n = (len < max_bytes) ? len : max_bytes;
    }
    return n;
}

static const char *get_sentence(int idx, size_t *len);

static bool is_ascii_sentence_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim_sentence_span(const char *text, size_t *start, size_t *len)
{
    while (*len > 0 && is_ascii_sentence_space(text[*start])) {
        (*start)++;
        (*len)--;
    }

    while (*len > 0 && is_ascii_sentence_space(text[*start + *len - 1])) {
        (*len)--;
    }
}

static bool has_suffix_bytes(const char *text, size_t len, const uint8_t *tail, size_t tail_len)
{
    if (len < tail_len) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)text + (len - tail_len);
    return memcmp(p, tail, tail_len) == 0;
}

static bool sentence_has_strong_terminal(const char *text, size_t len)
{
    static const uint8_t zh_period[] = {0xE3, 0x80, 0x82}; /* 。 */
    static const uint8_t zh_excl[] = {0xEF, 0xBC, 0x81};   /* ！ */
    static const uint8_t zh_q[] = {0xEF, 0xBC, 0x9F};      /* ？ */

    if (len == 0) {
        return false;
    }

    char last = text[len - 1];
    if (last == '.' || last == '!' || last == '?' || last == '\n') {
        return true;
    }

    if (has_suffix_bytes(text, len, zh_period, sizeof(zh_period)) ||
        has_suffix_bytes(text, len, zh_excl, sizeof(zh_excl)) ||
        has_suffix_bytes(text, len, zh_q, sizeof(zh_q))) {
        return true;
    }

    return false;
}

static bool is_common_cjk_punct(uint32_t cp)
{
    switch (cp) {
    case 0x3001: /* 、 */
    case 0x3002: /* 。 */
    case 0x300A: /* 《 */
    case 0x300B: /* 》 */
    case 0x300C: /* 「 */
    case 0x300D: /* 」 */
    case 0x300E: /* 『 */
    case 0x300F: /* 』 */
    case 0x3010: /* 【 */
    case 0x3011: /* 】 */
    case 0x3014: /* 〔 */
    case 0x3015: /* 〕 */
    case 0x3018: /* 〘 */
    case 0x3019: /* 〙 */
    case 0x301A: /* 〚 */
    case 0x301B: /* 〛 */
    case 0xFF01: /* ！ */
    case 0xFF02: /* ＂ */
    case 0xFF08: /* （ */
    case 0xFF09: /* ） */
    case 0xFF0C: /* ， */
    case 0xFF1A: /* ： */
    case 0xFF1B: /* ； */
    case 0xFF1F: /* ？ */
    case 0x2014: /* — */
    case 0x2018: /* ‘ */
    case 0x2019: /* ’ */
    case 0x201C: /* “ */
    case 0x201D: /* ” */
    case 0x2026: /* … */
        return true;
    default:
        return false;
    }
}

static bool decode_one_utf8(const char *s, size_t len, uint32_t *cp, size_t *used)
{
    if (!s || len == 0 || !cp || !used) {
        return false;
    }

    const uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80) {
        *cp = b0;
        *used = 1;
        return true;
    }

    if ((b0 & 0xE0) == 0xC0 && len >= 2) {
        *cp = ((uint32_t)(b0 & 0x1F) << 6) | ((uint32_t)s[1] & 0x3F);
        *used = 2;
        return true;
    }

    if ((b0 & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((uint32_t)(b0 & 0x0F) << 12) |
              (((uint32_t)s[1] & 0x3F) << 6) |
              ((uint32_t)s[2] & 0x3F);
        *used = 3;
        return true;
    }

    if ((b0 & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((uint32_t)(b0 & 0x07) << 18) |
              (((uint32_t)s[1] & 0x3F) << 12) |
              (((uint32_t)s[2] & 0x3F) << 6) |
              ((uint32_t)s[3] & 0x3F);
        *used = 4;
        return true;
    }

    *cp = b0;
    *used = 1;
    return true;
}

static bool sentence_has_spoken_content(const char *text, size_t len)
{
    size_t i = 0;

    while (i < len) {
        uint32_t cp = 0;
        size_t used = 0;
        if (!decode_one_utf8(text + i, len - i, &cp, &used) || used == 0) {
            break;
        }

        if (cp < 0x80) {
            unsigned char c = (unsigned char)cp;
            if (isalnum(c)) {
                return true;
            }
            if (isspace(c) || ispunct(c)) {
                i += used;
                continue;
            }
            return true;
        }

        if (is_common_cjk_punct(cp)) {
            i += used;
            continue;
        }

        return true;
    }

    return false;
}

static void cloud_batch_limits(size_t *max_bytes, int *max_sentences)
{
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (int_largest < 34000 || int_free < 80000) {
        *max_bytes = CLOUD_BATCH_MAX_BYTES_CRITICAL;
        *max_sentences = CLOUD_BATCH_MAX_SENTENCES_CRITICAL;
        return;
    }

    if (int_largest < 48000 || int_free < 100000) {
        *max_bytes = CLOUD_BATCH_MAX_BYTES_LOWMEM;
        *max_sentences = CLOUD_BATCH_MAX_SENTENCES_LOWMEM;
        return;
    }

    *max_bytes = CLOUD_BATCH_MAX_BYTES_NORMAL;
    *max_sentences = CLOUD_BATCH_MAX_SENTENCES_NORMAL;
}

static int build_cloud_sentence_batch(int start_idx, char *dst, size_t dst_size,
                                      int *consumed_sentences)
{
    int added = 0;
    int consumed = 0;
    size_t out_len = 0;
    size_t batch_max_bytes = 0;
    int batch_max_sentences = 0;

    if (dst_size == 0) {
        if (consumed_sentences) *consumed_sentences = 0;
        return 0;
    }

    cloud_batch_limits(&batch_max_bytes, &batch_max_sentences);
    if (batch_max_bytes > dst_size - 1) {
        batch_max_bytes = dst_size - 1;
    }

    dst[0] = '\0';

    for (int idx = start_idx; idx < s_num_sentences - 1; idx++) {
        size_t raw_len = 0;
        const char *raw = get_sentence(idx, &raw_len);
        if (!raw) {
            break;
        }

        size_t trim_start = 0;
        size_t trim_len = raw_len;
        trim_sentence_span(raw, &trim_start, &trim_len);

        if (trim_len == 0) {
            consumed++;
            continue;
        }

        if (!sentence_has_spoken_content(raw + trim_start, trim_len)) {
            consumed++;
            continue;
        }

        if (added > 0) {
            if (added >= batch_max_sentences) {
                break;
            }
            if (out_len + trim_len > batch_max_bytes) {
                break;
            }
        }

        size_t copy_len = trim_len;
        if (copy_len > dst_size - out_len - 1) {
            copy_len = dst_size - out_len - 1;
        }
        if (copy_len == 0) {
            break;
        }

        copy_len = utf8_safe_copy(dst + out_len, dst_size - out_len, raw + trim_start, copy_len);
        out_len += copy_len;
        dst[out_len] = '\0';
        consumed++;
        added++;

        if (added >= 2 && sentence_has_strong_terminal(raw + trim_start, trim_len)) {
            break;
        }

        if (copy_len < trim_len && added == 1) {
            break;
        }
    }

    if (consumed_sentences) {
        *consumed_sentences = consumed;
    }
    return added;
}

static const char *get_sentence(int idx, size_t *len)
{
    if (idx < 0 || idx >= s_num_sentences - 1) {
        *len = 0;
        return NULL;
    }
    *len = s_sentence_starts[idx + 1] - s_sentence_starts[idx];
    return s_file_content + s_sentence_starts[idx];
}

static bool load_and_display_file(int idx)
{
    if (!s_reading_buffers_ready || !s_file_content || !s_file_raw) {
        ESP_LOGE(TAG, "Reading buffers not available");
        s_file_loaded = false;
        return false;
    }

    if (idx < 0 || idx >= s_file_count) {
        s_file_loaded = false;
        return false;
    }

    sd_card_lock();

    char path[320];
    snprintf(path, sizeof(path), "/sdcard/%s", s_file_names[idx]);

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "stat failed: %s", path);
        sd_card_unlock();
        s_file_loaded = false;
        return false;
    }

    size_t fsize = st.st_size;
    if (fsize == 0) {
        ESP_LOGW(TAG, "Skip empty file: %s", s_file_names[idx]);
        sd_card_unlock();
        s_file_loaded = false;
        return false;
    }

    size_t orig_fsize = fsize;
    if (fsize > MAX_FILES_CONTENT - 1) {
        ESP_LOGW(TAG, "File too large: %llu bytes, truncated to %zu",
                 orig_fsize, MAX_FILES_CONTENT - 1);
        fsize = MAX_FILES_CONTENT - 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "fopen failed: %s", path);
        sd_card_unlock();
        s_file_loaded = false;
        return false;
    }

    size_t raw_len = fread(s_file_raw, 1, fsize, f);
    fclose(f);
    sd_card_unlock();
    if (raw_len == 0) {
        ESP_LOGW(TAG, "Skip unreadable/zero-read file: %s", s_file_names[idx]);
        s_file_loaded = false;
        return false;
    }
    s_content_len = normalize_text_to_utf8(s_file_raw, raw_len, s_file_content, MAX_FILES_CONTENT);
    if (s_content_len == 0) {
        ESP_LOGW(TAG, "Skip undecodable text file: %s", s_file_names[idx]);
        s_file_loaded = false;
        return false;
    }
    s_file_content[s_content_len] = '\0';

    s_num_sentences = split_sentences(s_file_content, s_sentence_starts, MAX_SENTENCES);
    s_current_sentence = 0;
    s_sentence_offset = 0;
    s_last_chunk_len = 0;
    s_file_loaded = true;
    s_current_file = idx;

    ESP_LOGI(TAG, "Loaded: %s (%d bytes, %d sentences)", s_file_names[idx], s_content_len, s_num_sentences);

    /* Show on screen */
    app_display_set_filename(s_file_names[idx]);
    utf8_safe_copy(s_display_buf, sizeof(s_display_buf), s_file_content, s_content_len);
    app_display_set_text(s_display_buf);
    app_display_set_mode(DISP_MODE_FILE_DONE);
    return true;
}

/* ── File scanner (sorted by name) ───────────────────────────────────── */

static int name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static bool is_temporary_txt_name(const char *name)
{
    if (!name || name[0] == '\0') return true;

    if (name[0] == '.') return true;
    if (name[0] == '~') return true;  /* tilde-prefixed temp/lock files */
    if (name[0] == '_') return true;  /* underscore-prefixed temp files */

    size_t n = strlen(name);
    if (n >= 4 && strcasecmp(name + (n - 4), ".tmp") == 0) return true;
    if (n >= 4 && strcasecmp(name + (n - 4), ".swp") == 0) return true;

    return false;
}

static bool filename_exists_ci(const char names[][MAX_FNAME_LEN], int count, const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcasecmp(names[i], name) == 0) return true;
    }
    return false;
}

static void scan_files(void)
{
    sd_card_lock();
    DIR *dir = opendir("/sdcard");
    if (!dir) {
        sd_card_unlock();
        ESP_LOGW(TAG, "Cannot open /sdcard");
        app_display_set_text("SD card not available.\nInsert TF card and try again.");
        return;
    }

    s_file_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_file_count < MAX_FILES) {
        if (is_temporary_txt_name(entry->d_name)) continue;

        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcasecmp(ext, ".txt") != 0) continue;

        if (filename_exists_ci(s_file_names, s_file_count, entry->d_name)) continue;

        strncpy(s_file_names[s_file_count], entry->d_name, MAX_FNAME_LEN - 1);
        s_file_names[s_file_count][MAX_FNAME_LEN - 1] = '\0';
        ESP_LOGI(TAG, "TXT candidate: %s", s_file_names[s_file_count]);
        s_file_count++;
    }
    closedir(dir);
    sd_card_unlock();

    qsort(s_file_names, s_file_count, MAX_FNAME_LEN, name_cmp);

    ESP_LOGI(TAG, "Found %d .txt files", s_file_count);

    if (s_file_count == 0) {
        app_display_set_text("No .txt files found.\nCopy files via USB then\npress KEY1 to scan.");
        return;
    }

    /* Load first readable non-empty file */
    for (int i = 0; i < s_file_count; i++) {
        if (load_and_display_file(i)) {
            return;
        }
    }

    app_display_set_text("No readable .txt content.\nCheck file encoding/flush\nand press KEY1 again.");
}

/* ── Playback control ────────────────────────────────────────────────── */

/* Callback-based sentence chaining — registered as TTS completion
 * callback so advancement runs directly from worker task context,
 * eliminating the poll-task race condition. */
static void on_tts_finished(void);

static void speak_current_sentence(void)
{
    STATE_LOCK();
    if (!s_file_loaded || s_current_sentence >= s_num_sentences - 1) {
        STATE_UNLOCK();
        return;
    }

    size_t slen;
    const char *stext = get_sentence(s_current_sentence, &slen);
    if (!stext || slen == 0) {
        STATE_UNLOCK();
        return;
    }

    if (s_sentence_offset >= slen) {
        s_sentence_offset = 0;
    }

    while (s_sentence_offset < slen && ((unsigned char)stext[s_sentence_offset] & 0xC0) == 0x80) {
        s_sentence_offset++;
    }
    if (s_sentence_offset >= slen) {
        s_sentence_offset = 0;
        s_current_sentence++;
        if (s_current_sentence < s_num_sentences - 1) {
            STATE_UNLOCK();
            speak_current_sentence();
        } else {
            s_playing = false;
            STATE_UNLOCK();
            app_display_set_mode(DISP_MODE_FILE_DONE);
            app_display_set_text(
                "File finished.\n"
                "KEY4: Next file\n"
                "KEY2: Main menu");
        }
        return;
    }

    size_t remaining = slen - s_sentence_offset;
    size_t chunk_len = tts_pick_chunk_len(stext + s_sentence_offset, remaining, TTS_RUNTIME_MAX_BYTES);
    if (chunk_len == 0) {
        ESP_LOGW(TAG, "empty chunk (sentence=%d)", s_current_sentence);
        s_playing = false;
        STATE_UNLOCK();
        app_display_set_mode(DISP_MODE_PAUSED);
        app_display_set_text("Audio start failed.\nCheck TTS/codec init logs.");
        return;
    }

    STATE_UNLOCK();

    char sentence[512];
    utf8_safe_copy(sentence, sizeof(sentence), stext + s_sentence_offset, chunk_len);
    s_last_chunk_len = chunk_len;

    esp_err_t ret = app_tts_speak_cb(sentence, on_tts_finished);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "app_tts_speak failed: %s", esp_err_to_name(ret));
        s_playing = false;
        app_display_set_mode(DISP_MODE_PAUSED);
        app_display_set_text("Audio start failed.\nCheck TTS/codec init logs.");
        return;
    }
    ESP_LOGI(TAG, "TTS sentence queued (%d bytes)", (int)strlen(sentence));

    app_display_set_progress(s_current_sentence + 1, s_num_sentences - 1);
}

static void speak_current_sentence_cloud(void)
{
    char sentence[512];
    int consumed_sentences = 0;

    STATE_LOCK();
    if (!s_file_loaded || s_current_sentence >= s_num_sentences - 1) {
        STATE_UNLOCK();
        return;
    }

    int batched = build_cloud_sentence_batch(s_current_sentence, sentence, sizeof(sentence),
                                             &consumed_sentences);
    if (batched == 0) {
        s_current_sentence += consumed_sentences;
        s_cloud_batch_sentences = 0;
        bool done = (s_current_sentence >= s_num_sentences - 1);
        if (done) {
            s_playing = false;
        }
        STATE_UNLOCK();

        if (done) {
            app_display_set_mode(DISP_MODE_FILE_DONE);
            app_display_set_text(
                "File finished.\n"
                "KEY4: Next file\n"
                "KEY2: Main menu");
        }
        return;
    }

    s_cloud_batch_sentences = consumed_sentences;
    STATE_UNLOCK();

    esp_err_t ret = app_cloud_tts_speak(sentence, on_tts_finished);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "cloud TTS failed, fallback to local");
        esp_err_t local_ret = app_tts_speak_cb(sentence, on_tts_finished);
        if (local_ret != ESP_OK) {
            s_playing = false;
            app_display_set_mode(DISP_MODE_PAUSED);
            app_display_set_text("TTS failed.\nCheck network/config.");
        }
    }

    if (ret == ESP_OK) {
        int next_idx;
        char prefetch_sentence[512];
        int prefetch_consumed = 0;
        size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        oom_level_t oom_level = app_oom_get_level();

        STATE_LOCK();
        next_idx = s_current_sentence + consumed_sentences;
        STATE_UNLOCK();

        bool can_prefetch = (next_idx < s_num_sentences - 1) &&
                            (int_largest >= 30000) &&
                            (int_free >= 70000) &&
                            (oom_level <= OOM_LEVEL_LVGL_BUFFER_HALF);

        if (can_prefetch) {
            int batched_next = build_cloud_sentence_batch(next_idx,
                                                          prefetch_sentence,
                                                          sizeof(prefetch_sentence),
                                                          &prefetch_consumed);
            if (batched_next > 0) {
                esp_err_t prefetch_ret = app_cloud_tts_prefetch(prefetch_sentence);
                if (prefetch_ret != ESP_OK) {
                    ESP_LOGD(TAG, "cloud prefetch failed: %s", esp_err_to_name(prefetch_ret));
                }
            }
        } else {
            ESP_LOGD(TAG,
                     "cloud prefetch skipped (next=%d int_free=%u int_largest=%u oom=%d)",
                     next_idx,
                     (unsigned)int_free,
                     (unsigned)int_largest,
                     (int)oom_level);
        }
    }

    app_display_set_progress(s_current_sentence + consumed_sentences, s_num_sentences - 1);
}

static void on_tts_finished(void)
{
    STATE_LOCK();
    /* Pause/stop during callback — don't advance */
    if (!s_playing) { STATE_UNLOCK(); return; }

    size_t slen = 0;
    if (app_tts_get_channel() == TTS_CHANNEL_CLOUD) {
        int advance = (s_cloud_batch_sentences > 0) ? s_cloud_batch_sentences : 1;
        s_sentence_offset = 0;
        s_last_chunk_len = 0;
        s_current_sentence += advance;
        s_cloud_batch_sentences = 0;
    } else {
        (void)get_sentence(s_current_sentence, &slen);

        if (s_last_chunk_len > 0 && s_sentence_offset + s_last_chunk_len < slen) {
            s_sentence_offset += s_last_chunk_len;
        } else {
            s_sentence_offset = 0;
            s_current_sentence++;
        }
    }

    /* Update display to follow reading progress */
    bool need_advance = false;
    if (s_current_sentence < s_num_sentences - 1) {
        const char *stext = get_sentence(s_current_sentence, &slen);
        if (stext && slen > 0 && s_sentence_offset == 0) {
            const char *display_ptr = stext + s_sentence_offset;
            ptrdiff_t start_off = display_ptr - s_file_content;
            if (start_off < 0 || (size_t)start_off > s_content_len) {
                start_off = (ptrdiff_t)s_content_len;
            }
            size_t remaining = s_content_len - (size_t)start_off;
            STATE_UNLOCK();
            utf8_safe_copy(s_display_buf, sizeof(s_display_buf), display_ptr, remaining);
            app_display_set_text(s_display_buf);
            STATE_LOCK();
        }
        /* Cloud TTS advances via btn_evt queue to avoid blocking
         * HTTP calls in the callback chain. Dispatch immediately. */
        if (app_tts_get_channel() == TTS_CHANNEL_CLOUD) {
            STATE_UNLOCK();
            button_msg_t adv = { .key_id = 5, .event = BTN_EVENT_SHORT_PRESS };
            xQueueSend(s_button_event_queue, &adv, 0);
            return;
        }
        need_advance = true;
    } else {
        s_playing = false;
        STATE_UNLOCK();
        app_display_set_mode(DISP_MODE_FILE_DONE);
        app_display_set_text(
            "File finished.\n"
            "KEY4: Next file\n"
            "KEY2: Main menu");
        return;
    }
    STATE_UNLOCK();

    /* Unlocked before calling speak_current_sentence to avoid
     * recursive mutex deadlock (speak_current_sentence takes STATE_LOCK). */
    if (need_advance) {
        speak_current_sentence();
    }
}

/* ── Action callbacks ───────────────────────────────────────────────── */

static void beep(void)
{
    app_tts_play_beep();
}

static void do_scan_files(void)
{
    beep();
    app_display_set_mode(DISP_MODE_SCANNING);
    app_display_set_text("Scanning...");
    scan_files();
    mem_snapshot("files_scanned");
}

static void do_play(void)
{
    beep();
    STATE_LOCK();
    /* If playback finished, restart from beginning */
    if (s_current_sentence >= s_num_sentences - 1) {
        s_current_sentence = 0;
        s_sentence_offset = 0;
        s_last_chunk_len = 0;
        s_cloud_batch_sentences = 0;
    }
    s_playing = true;
    STATE_UNLOCK();
    app_display_set_mode(DISP_MODE_READING);
    mem_snapshot("play_start_local");
    speak_current_sentence();
}

static void do_pause(void)
{
    beep();
    STATE_LOCK();
    s_playing = false;
    s_last_chunk_len = 0;
    s_cloud_batch_sentences = 0;
    STATE_UNLOCK();
    app_tts_stop();
    app_display_set_mode(DISP_MODE_PAUSED);
}

static void do_toggle_play(void)
{
    if (!s_playing) {
        do_play();
    } else {
        do_pause();
    }
}

static void do_play_cloud(void)
{
    beep();
    STATE_LOCK();
    if (s_current_sentence >= s_num_sentences - 1) {
        s_current_sentence = 0;
        s_sentence_offset = 0;
        s_last_chunk_len = 0;
        s_cloud_batch_sentences = 0;
    }
    s_playing = true;
    STATE_UNLOCK();
    app_display_set_mode(DISP_MODE_CLOUD_READING);
    mem_snapshot("play_start_cloud");
    speak_current_sentence_cloud();
}

static void do_pause_cloud(void)
{
    beep();
    STATE_LOCK();
    s_playing = false;
    s_cloud_batch_sentences = 0;
    STATE_UNLOCK();
    app_cloud_tts_stop();
    app_display_set_mode(DISP_MODE_PAUSED);
}

static void do_toggle_play_cloud(void)
{
    if (!s_playing) {
        do_play_cloud();
    } else {
        do_pause_cloud();
    }
}

static void do_skip(void)
{
    if (s_playing || !s_file_loaded) return;
    beep();
    STATE_LOCK();
    s_sentence_offset = 0;
    s_last_chunk_len = 0;
    s_cloud_batch_sentences = 0;
    s_current_sentence += 5;
    if (s_current_sentence >= s_num_sentences - 1) {
        s_current_sentence = 0;
    }
    {
        size_t slen;
        const char *stext = get_sentence(s_current_sentence, &slen);
        if (stext && slen > 0) {
            STATE_UNLOCK();
            size_t remaining = s_content_len - (stext - s_file_content);
            utf8_safe_copy(s_display_buf, sizeof(s_display_buf), stext, remaining);
            app_display_set_text(s_display_buf);
        } else {
            STATE_UNLOCK();
        }
    }
}

static void do_next_file(void)
{
    STATE_LOCK();
    s_playing = false;
    STATE_UNLOCK();
    app_tts_stop();
    beep();
    if (s_file_count > 0) {
        int next = (s_current_file + 1) % s_file_count;
        bool loaded = false;
        for (int i = 0; i < s_file_count; i++) {
            int idx = (next + i) % s_file_count;
            if (load_and_display_file(idx)) {
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            app_display_set_text("No readable .txt content.\nPress KEY1 to rescan.");
        }
    }
}

/* ── Main menu & mode switching ────────────────────────────────────── */

static void show_main_menu(void)
{
    s_op_mode = MODE_MAIN_MENU;
    if (s_card_available) {
        if (g_app_config.tts_api_key[0] && g_app_config.wifi_ssid[0]) {
            app_display_set_mode_text(DISP_MODE_USB_MSC,
                "[KEY1] Copy files via USB\n"
                "[KEY2] Local TTS reading\n"
                "[KEY3] Cloud TTS reading");
        } else {
            app_display_set_mode_text(DISP_MODE_USB_MSC,
                "[KEY1] Copy files via USB\n"
                "[KEY2] Read .txt files");
        }
    } else {
        app_display_set_mode_text(DISP_MODE_USB_MSC,
            "SD card not detected.\n"
            "Insert TF card and restart.");
    }
}

static void enter_usb_mode(void)
{
    beep();
    s_op_mode = MODE_USB_COPY;
    app_display_set_mode(DISP_MODE_USB_MSC);

    if (!s_usb_started && s_card && s_card_available) {
        /* First time: init USB PHY, TinyUSB, and background task */
        app_display_set_text(
            "Starting USB...\n"
            "Please wait for PC to\n"
            "detect the drive.");
        esp_err_t ret = tusb_msc_init(s_card);
        s_usb_started = (ret == ESP_OK);
        if (ret != ESP_OK) {
            app_display_set_text(
                "USB init failed.\n"
                "Press KEY1 for menu.");
            return;
        }
        mem_snapshot("usb_mode_enter");
        return;
    }

    /* Re-entering USB mode: resume USB task and reset eject flag */
    tusb_msc_resume();
    tusb_msc_reset_eject();
    if (tusb_msc_is_connected()) {
        app_display_set_text(
            "USB drive ready.\n"
            "Copy files to TF card.\n"
            "Press KEY1 to return.");
    } else {
        app_display_set_text(
            "USB mode active.\n"
            "Connect USB cable,\n"
            "then PC should detect drive.");
    }
}

static void enter_reading_mode(void)
{
    beep();
    if (!s_reading_buffers_ready) {
        app_display_set_text(
            "Memory insufficient for\n"
            "reading buffers.\n"
            "Use USB mode or reboot.");
        return;
    }

    if (!s_card_available) {
        app_display_set_text(
            "SD card not detected.\n"
            "Please insert TF card\n"
            "and restart.");
        return;
    }

    /* USB cable still plugged in: force disconnect so the host stops
     * sending SCSI commands. This allows FATFS to safely access the SD
     * card during reading without bus conflicts. USB reconnects
     * automatically when returning to the main menu. */
    if (s_usb_started && tusb_msc_is_connected()) {
        tusb_msc_disconnect();
        s_usb_disconnected_for_reading = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "USB disconnected for reading mode");
    }

    s_op_mode = MODE_READING;
    app_display_set_text(
        "KEY1: Scan files\n"
        "KEY2: Play/Pause\n"
        "KEY3: Skip ahead\n"
        "KEY4: Next file\n"
        "(hold KEY1 for menu)");
}

static void enter_cloud_reading_mode(void)
{
    beep();
    if (!s_reading_buffers_ready) {
        app_display_set_text(
            "Memory insufficient for\n"
            "reading buffers.\n"
            "Use USB mode or reboot.");
        return;
    }

    if (!s_card_available) {
        app_display_set_text(
            "SD card not detected.\n"
            "Please insert TF card\n"
            "and restart.");
        return;
    }
    if (!app_wifi_is_connected()) {
        app_display_set_text(
            "WiFi not connected.\n"
            "Check config.ini\n"
            "and try again.");
        return;
    }
    if (!g_app_config.tts_api_key[0]) {
        app_display_set_text(
            "Cloud TTS not configured.\n"
            "Set TTS_API_KEY in\n"
            "config.ini.");
        return;
    }

    /* Force USB disconnect so FATFS can safely access SD card */
    if (s_usb_started && tusb_msc_is_connected()) {
        tusb_msc_disconnect();
        s_usb_disconnected_for_reading = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "USB disconnected for cloud reading mode");
    }

    s_op_mode = MODE_READING;
    app_tts_set_channel(TTS_CHANNEL_CLOUD);
    app_display_set_text(
        "Cloud TTS mode\n"
        "KEY1: Scan files\n"
        "KEY2: Play/Pause\n"
        "KEY3: Skip ahead\n"
        "KEY4: Next file\n"
        "(hold KEY1 for menu)");
}

static void return_to_main_menu(void)
{
    beep();
    /* Suspend USB task so tud_task() doesn't race during mode switch */
    tusb_msc_suspend();
    STATE_LOCK();
    s_playing = false;
    STATE_UNLOCK();

    /* Avoid resetting TTS queues when not actively speaking. */
    if (app_tts_is_busy()) {
        app_tts_stop();
    }
    if (app_cloud_tts_is_busy()) {
        app_cloud_tts_stop();
    }

    /* Reconnect USB if we disconnected it for reading mode */
    if (s_usb_disconnected_for_reading) {
        tusb_msc_connect();
        s_usb_disconnected_for_reading = false;
        /* Give time for host to enumerate the reconnected device */
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    show_main_menu();
    mem_snapshot("return_main_menu");
}

/* ── Boot completion ────────────────────────────────────────────────── */

static void do_boot_complete(void)
{
    s_system_ready = true;
    show_main_menu();
}

/* ── Button callback ─────────────────────────────────────────────────── */

static void process_button_event(int key_id, btn_event_t event)
{
    if (!s_system_ready) {
        app_display_set_text(
            "System starting up...\n"
            "Please wait a moment.");
        return;
    }

    if (event == BTN_EVENT_LONG_PRESS && key_id == 1 && s_op_mode == MODE_READING) {
        /* Long-press KEY1 in reading mode returns to main menu */
        return_to_main_menu();
        return;
    }

    if (event != BTN_EVENT_SHORT_PRESS) return;

    switch (s_op_mode) {

    case MODE_MAIN_MENU:
        if (key_id == 1) {
            if (!key1_mode_switch_allowed()) {
                ESP_LOGW(TAG, "KEY1 ignored: mode switch guard");
                break;
            }
            enter_usb_mode();
        } else if (key_id == 2) {
            app_tts_set_channel(TTS_CHANNEL_LOCAL);
            enter_reading_mode();
        } else if (key_id == 3 && g_app_config.tts_api_key[0]) {
            enter_cloud_reading_mode();
        }
        break;

    case MODE_USB_COPY:
        if (key_id == 1) {
            if (!key1_mode_switch_allowed()) {
                ESP_LOGW(TAG, "KEY1 ignored: mode switch guard");
                break;
            }
            return_to_main_menu();
        }
        break;

    case MODE_READING:
        switch (key_id) {
        case 1: /* KEY1: scan files */
            if (!s_playing) {
                do_scan_files();
            }
            break;

        case 2: /* KEY2: play/pause / menu */
            if (!s_file_loaded) break;
            /* If file is done (not playing, not paused), KEY2 returns to menu */
            if (!s_playing && s_current_sentence >= s_num_sentences - 1) {
                return_to_main_menu();
                break;
            }
            if (!s_tts_available && app_tts_get_channel() != TTS_CHANNEL_CLOUD) {
                app_display_set_mode(DISP_MODE_PAUSED);
                app_display_set_text("TTS not available.\nCheck voice init/logs.");
                break;
            }
            if (app_tts_get_channel() == TTS_CHANNEL_CLOUD) {
                do_toggle_play_cloud();
            } else {
                do_toggle_play();
            }
            break;

        case 3: /* KEY3: skip ahead */
            if (s_playing || !s_file_loaded) break;
            do_skip();
            break;

        case 4: /* KEY4: next file */
            if (!s_file_loaded) break;
            do_next_file();
            break;

        case 5: /* Internal: advance cloud TTS (from poll task safety net) */
            if (s_playing && app_tts_get_channel() == TTS_CHANNEL_CLOUD) {
                speak_current_sentence_cloud();
            }
            break;

        case 6: /* Boot test: system-ready prompt via cloud TTS */
            if (g_app_config.tts_api_key[0]) {
                /* Will fallback to local TTS if cloud unavailable */
                app_cloud_tts_speak("系统已经准备好", NULL);
            }
            break;

        default:
            break;
        }
        break;
    }
}

static bool enqueue_button_event(int key_id, btn_event_t event, const char *source)
{
    if (!s_button_event_queue) {
        return false;
    }

    button_msg_t msg = {
        .key_id = key_id,
        .event = event,
    };

    if (xQueueSend(s_button_event_queue, &msg, 0) != pdTRUE) {
        static uint32_t drop_cnt = 0;
        drop_cnt++;
        if ((drop_cnt <= 5) || ((drop_cnt % 50) == 0)) {
            ESP_LOGW(TAG, "btn_evt queue full: drop key=%d evt=%d src=%s cnt=%u",
                     key_id, (int)event, source ? source : "?", (unsigned)drop_cnt);
        }
        return false;
    }

    return true;
}

static void on_button_event(int key_id, btn_event_t event)
{
    if (!s_button_event_queue) return;
    if (!s_system_ready) return;

    (void)enqueue_button_event(key_id, event, "btn_scan");
}

static void button_event_task(void *arg)
{
    (void)arg;
    button_msg_t msg;
    uint32_t loop_cnt = 0;

    while (1) {
        loop_cnt++;
        if (loop_cnt % 600 == 0) {
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "[STACK] task=btn_evt watermark=%u words", (unsigned)watermark);
        }
        if (xQueueReceive(s_button_event_queue, &msg, portMAX_DELAY) == pdTRUE) {
            process_button_event(msg.key_id, msg.event);
        }
    }
}

/* ── TTS polling task ────────────────────────────────────────────────── */
/* Sentence advancement is callback-driven from the worker task.
 * Poll task is a safety net only: detects chain breakage after 5s idle. */

static void poll_task(void *arg)
{
    (void)arg;
    static bool usb_ready_shown = false;
    int idle_100ms = 0;
    int tick = 0;

    while (1) {
        /* Poll task heartbeat — proves this task is running */
        {
            static int heartbeat = 0;
            if (++heartbeat % 10 == 0) {
                ESP_LOGI(TAG, "poll alive (%d)", heartbeat);
            }
        }

        /* When USB starts but host hasn't mounted yet, wait for mount */
        if (s_usb_started && !usb_ready_shown) {
            if (tusb_msc_is_connected()) {
                usb_ready_shown = true;
                if (s_op_mode == MODE_USB_COPY) {
                    app_display_set_text(
                        "USB drive ready.\n"
                        "Copy files to TF card.\n"
                        "Press KEY1 to return.");
                }
            }
        }
        /* Reset flag when USB is stopped */
        if (!s_usb_started) {
            usb_ready_shown = false;
        }

        /* Safety net: if playing but TTS idle for 5s, re-chain */
        if (s_playing) {
            bool is_cloud = (app_tts_get_channel() == TTS_CHANNEL_CLOUD);
            bool busy = is_cloud ? app_cloud_tts_is_busy() : app_tts_is_busy();
            if (!busy) {
                idle_100ms++;
                if (idle_100ms > 10) {
                    idle_100ms = 0;
                    STATE_LOCK();
                    int sent = s_current_sentence;
                    int total = s_num_sentences - 1;
                    STATE_UNLOCK();
                    ESP_LOGW(TAG, "Chain broken, re-queuing sentence %d/%d",
                             sent, total);
                    if (is_cloud) {
                        /* Cloud TTS blocking HTTP must run in btn_evt task, not here */
                        (void)enqueue_button_event(5, BTN_EVENT_SHORT_PRESS, "poll_chain");
                    } else {
                        speak_current_sentence();
                    }
                }
            } else {
                idle_100ms = 0;
            }
        } else {
            idle_100ms = 0;
        }

        tick++;
        if (tick % 100 == 0) {
            mem_snapshot("poll_10s");
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "[STACK] task=tts_poll watermark=%u words", (unsigned)watermark);
#if ENABLE_HEAP_INTEGRITY_SCAN
            heap_caps_check_integrity_all(true);
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void on_wifi_event(bool connected)
{
    mem_snapshot(connected ? "wifi_connected" : "wifi_disconnected");
    if (connected && s_system_ready && g_app_config.tts_api_key[0]) {
        /* Boot test: play system-ready prompt via cloud TTS (routed through btn_evt) */
        (void)enqueue_button_event(6, BTN_EVENT_SHORT_PRESS, "wifi_evt");
    }
}

/* ── Initialization ──────────────────────────────────────────────────── */

static void init_step(const char *label, esp_err_t result)
{
    size_t len = strlen(s_init_log);
    const char *status = (result == ESP_OK) ? "OK" : "FAIL";
    snprintf(s_init_log + len, sizeof(s_init_log) - len, "%s %s\n", label, status);
    app_display_set_text(s_init_log);
    vTaskDelay(pdMS_TO_TICKS(400));
}

void app_main(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGW(TAG, "Reset reason: %s (%d)", reset_reason_str(rr), (int)rr);
    ESP_LOGI(TAG, "=== Voice Reader v2.1.0 ===");
    esp_err_t ret;
    ret = nvs_flash_init();
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
    }
    mem_snapshot("boot_start");

    /* Hardware init (before LVGL — no display updates yet) */
    ret = i2c_bus_init();                    ESP_ERROR_CHECK(ret);
    ret = xl9555_init();                     ESP_ERROR_CHECK(ret);
    ret = bsp_spi2_lcd_init();               ESP_ERROR_CHECK(ret);

    /* SD card */
    sdmmc_card_t *card = NULL;
    ret = sd_card_init(&card);
    if (ret == ESP_OK) {
        s_card_available = true;
        s_card = card;
    } else {
        s_card_available = false;
        s_card = NULL;
    }
    s_card_available = (ret == ESP_OK);
    mem_snapshot("sd_init_done");

    /* Allocate large file buffers from PSRAM to conserve internal SRAM */
    s_file_content = (char *)heap_caps_malloc(MAX_FILES_CONTENT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_file_raw = (uint8_t *)heap_caps_malloc(MAX_FILES_CONTENT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_file_names = (char (*)[MAX_FNAME_LEN])heap_caps_malloc(
        MAX_FILES * MAX_FNAME_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_file_names) {
        s_file_names = (char (*)[MAX_FNAME_LEN])heap_caps_malloc(
            MAX_FILES * MAX_FNAME_LEN, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_reading_buffers_ready = (s_file_content != NULL && s_file_raw != NULL && s_file_names != NULL);
    if (s_reading_buffers_ready) {
        memset(s_file_content, 0, MAX_FILES_CONTENT);
        memset(s_file_raw, 0, MAX_FILES_CONTENT);
        memset(s_file_names, 0, MAX_FILES * MAX_FNAME_LEN);
        ESP_LOGI(TAG, "File buffers allocated");
    } else {
        ESP_LOGE(TAG, "Reading buffers unavailable, reading mode disabled");
        if (s_file_content) {
            heap_caps_free(s_file_content);
            s_file_content = NULL;
        }
        if (s_file_raw) {
            heap_caps_free(s_file_raw);
            s_file_raw = NULL;
        }
        if (s_file_names) {
            heap_caps_free(s_file_names);
            s_file_names = NULL;
        }
    }
    mem_snapshot("file_buffers_ready");

    /* LVGL + LCD (display now available) — hardware init complete */
    ESP_ERROR_CHECK(app_display_init());

    /* ── Create all FreeRTOS objects after hardware init, before software init (§6.1) ── */

    /* State mutex for playback variables (§1.1) */
    s_state_mutex = xSemaphoreCreateMutexStatic(&s_state_mutex_buffer);

    /* Button event queue (static) */
    s_button_event_queue = xQueueCreateStatic(BTN_EVENT_QUEUE_LEN, sizeof(button_msg_t),
                                               s_btn_queue_storage, &s_btn_queue_buf);
    ESP_ERROR_CHECK(s_button_event_queue ? ESP_OK : ESP_ERR_NO_MEM);

    /* Poll task (static) */
    xTaskCreateStatic(poll_task, "tts_poll", POLL_TASK_STACK,
                      NULL, 3, s_poll_stack, &s_poll_tcb);

    /* Button event processing task (static) */
    xTaskCreateStatic(button_event_task, "btn_evt",
                      BTN_EVT_TASK_STACK, NULL, 2,
                      s_btn_evt_stack, &s_btn_evt_tcb);

    /* Show init log on screen (accumulated, line by line) */
    s_init_log[0] = '\0';
    init_step("Hardware init", ESP_OK);
    init_step("SD card", s_card_available ? ESP_OK : ESP_FAIL);

    /* Load config from SD card */
    memset(&g_app_config, 0, sizeof(g_app_config));
    if (s_card_available) {
        if (app_config_load(&g_app_config) == ESP_OK) {
            init_step("Config loaded", ESP_OK);
            mem_snapshot("config_loaded");
            if (g_app_config.wifi_ssid[0]) {
                app_wifi_init();
                app_wifi_set_callback(on_wifi_event);
                app_wifi_connect(g_app_config.wifi_ssid, g_app_config.wifi_pass);
                init_step("WiFi init", ESP_OK);
                mem_snapshot("wifi_connect_started");
            }
        } else {
            init_step("Config (no SD)", ESP_ERR_NOT_FOUND);
        }
    }

    /* USB MSC (deferred — starts after user presses a key) */
    s_system_ready = false;

    /* TTS engine */
    ret = app_tts_init();
    s_tts_available = (ret == ESP_OK);
    init_step("TTS engine", ret);
    mem_snapshot("tts_init_done");

    /* Cloud TTS engine — only if API credentials available */
    if (g_app_config.tts_api_key[0]) {
        ret = app_cloud_tts_init();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Cloud TTS ready");
            mem_snapshot("cloud_tts_ready");
        }
    }

    /* Buttons (init after TTS so beep is available).
     * Retry up to 3 times with 20ms delay — I2C to XL9555 may be
     * momentarily busy after ES8388/other I2C activity. */
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = app_buttons_init(on_button_event);
        if (ret == ESP_OK) {
            app_buttons_scan_start();
            break;
        }
        ESP_LOGW(TAG, "Button init attempt %d/3 failed: %s",
                 attempt + 1, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* Boot reason in filename */
    snprintf(s_boot_reason, sizeof(s_boot_reason), "Boot: %s", reset_reason_str(rr));
    app_display_set_filename(s_boot_reason);
    app_display_set_mode(DISP_MODE_USB_MSC);

    /* Boot — show main menu */
    {
        size_t len = strlen(s_init_log);
        snprintf(s_init_log + len, sizeof(s_init_log) - len, "Ready.\n");
        app_display_set_text(s_init_log);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    do_boot_complete();
    mem_snapshot("boot_ready");

    ESP_LOGI(TAG, "Ready");

    /* Boot TTS announcement — verify audio path */
    if (s_tts_available) {
        app_tts_set_channel(TTS_CHANNEL_LOCAL);
        app_tts_speak("系统已经准备好");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
