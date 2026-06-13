#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include <dirent.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "i2c_bus.h"
#include "xl9555.h"
#include "bsp_spi.h"
#include "board_pins.h"
#include "sd_card.h"
#include "tusb_msc.h"
#include "es8388.h"
#include "app_buttons.h"
#include "app_tts.h"
#include "app_display.h"

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

static char s_file_names[MAX_FILES][MAX_FNAME_LEN];
static int s_file_count = 0;
static int s_current_file = -1;
static char s_file_content[MAX_FILES_CONTENT] = {0};
static uint8_t s_file_raw[MAX_FILES_CONTENT] = {0};
static char s_display_buf[MAX_DISPLAY_TEXT] = {0};
static size_t s_content_len = 0;
static bool s_file_loaded = false;
static QueueHandle_t s_app_event_queue = NULL;

typedef enum {
    APP_EVT_BUTTON = 0,
    APP_EVT_TTS_DONE,
    APP_EVT_TTS_CHAIN_RESUME,
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    int key_id;
    btn_event_t button_event;
} app_event_t;

/* Sentence boundaries */
#define MAX_SENTENCES 256
#define TTS_RUNTIME_MAX_BYTES 72
static size_t s_sentence_starts[MAX_SENTENCES];
static int s_num_sentences = 0;
static int s_current_sentence = 0;
static size_t s_sentence_offset = 0;
static size_t s_last_chunk_len = 0;

/* Playback state */
static bool s_playing = false;
static bool s_system_ready = false;
static bool s_tts_available = false;
static bool s_card_available = false;

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

static void init_step(const char *label, esp_err_t result);

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
    if (idx < 0 || idx >= s_file_count) {
        s_file_loaded = false;
        return false;
    }

    char path[320];
    snprintf(path, sizeof(path), "/sdcard/%s", s_file_names[idx]);

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "stat failed: %s", path);
        s_file_loaded = false;
        return false;
    }

    size_t fsize = st.st_size;
    if (fsize == 0) {
        ESP_LOGW(TAG, "Skip empty file: %s", s_file_names[idx]);
        s_file_loaded = false;
        return false;
    }

    size_t orig_fsize = fsize;
    if (fsize > sizeof(s_file_content) - 1) {
        ESP_LOGW(TAG, "File too large: %llu bytes, truncated to %zu",
                 orig_fsize, sizeof(s_file_content) - 1);
        fsize = sizeof(s_file_content) - 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "fopen failed: %s", path);
        s_file_loaded = false;
        return false;
    }

    size_t raw_len = fread(s_file_raw, 1, fsize, f);
    fclose(f);
    if (raw_len == 0) {
        ESP_LOGW(TAG, "Skip unreadable/zero-read file: %s", s_file_names[idx]);
        s_file_loaded = false;
        return false;
    }
    s_content_len = normalize_text_to_utf8(s_file_raw, raw_len, s_file_content, sizeof(s_file_content));
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
    DIR *dir = opendir("/sdcard");
    if (!dir) {
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

/* TTS completion callback only posts an event. Playback state advancement
 * is serialized in app_event_task to avoid cross-task race conditions. */
static void on_tts_finished(void);
static void handle_tts_done_event(void);

static void speak_current_sentence(void)
{
    if (!s_file_loaded || s_current_sentence >= s_num_sentences - 1) return;

    size_t slen;
    const char *stext = get_sentence(s_current_sentence, &slen);
    if (!stext || slen == 0) return;

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
            speak_current_sentence();
        } else {
            s_playing = false;
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
        app_display_set_mode(DISP_MODE_PAUSED);
        app_display_set_text("Audio start failed.\nCheck TTS/codec init logs.");
        return;
    }

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

static void on_tts_finished(void)
{
    if (!s_app_event_queue) return;

    app_event_t msg = {
        .type = APP_EVT_TTS_DONE,
    };
    (void)xQueueSend(s_app_event_queue, &msg, 0);
}

static void handle_tts_done_event(void)
{
    /* Pause/stop while callback was in flight — don't advance */
    if (!s_playing) return;

    size_t slen = 0;
    (void)get_sentence(s_current_sentence, &slen);

    if (s_last_chunk_len > 0 && s_sentence_offset + s_last_chunk_len < slen) {
        s_sentence_offset += s_last_chunk_len;
    } else {
        s_sentence_offset = 0;
        s_current_sentence++;
    }

    /* Update display to follow reading progress */
    if (s_current_sentence < s_num_sentences - 1) {
        const char *stext = get_sentence(s_current_sentence, &slen);
        if (stext && slen > 0 && s_sentence_offset == 0) {
            const char *display_ptr = stext + s_sentence_offset;
            ptrdiff_t start_off = display_ptr - s_file_content;
            if (start_off < 0 || (size_t)start_off > s_content_len) {
                start_off = (ptrdiff_t)s_content_len;
            }
            size_t remaining = s_content_len - (size_t)start_off;
            utf8_safe_copy(s_display_buf, sizeof(s_display_buf), display_ptr, remaining);
            app_display_set_text(s_display_buf);
        }
        speak_current_sentence();
    } else {
        /* File finished */
        s_playing = false;
        app_display_set_mode(DISP_MODE_FILE_DONE);
        app_display_set_text(
            "File finished.\n"
            "KEY4: Next file\n"
            "KEY2: Main menu");
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
}

static void do_play(void)
{
    beep();
    /* If playback finished, restart from beginning */
    if (s_current_sentence >= s_num_sentences - 1) {
        s_current_sentence = 0;
        s_sentence_offset = 0;
        s_last_chunk_len = 0;
    }
    s_playing = true;
    app_display_set_mode(DISP_MODE_READING);
    speak_current_sentence();
}

static void do_pause(void)
{
    beep();
    s_playing = false;
    s_last_chunk_len = 0;
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

static void do_skip(void)
{
    if (s_playing || !s_file_loaded) return;
    beep();
    s_sentence_offset = 0;
    s_last_chunk_len = 0;
    s_current_sentence += 5;
    if (s_current_sentence >= s_num_sentences - 1) {
        s_current_sentence = 0;
    }
    {
        size_t slen;
        const char *stext = get_sentence(s_current_sentence, &slen);
        if (stext && slen > 0) {
            size_t remaining = s_content_len - (stext - s_file_content);
            utf8_safe_copy(s_display_buf, sizeof(s_display_buf), stext, remaining);
            app_display_set_text(s_display_buf);
        }
    }
}

static void do_next_file(void)
{
    s_playing = false;
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
    app_display_set_mode(DISP_MODE_USB_MSC);
    if (s_card_available) {
        app_display_set_text(
            "[KEY1] Copy files via USB\n"
            "[KEY2] Read .txt files");
    } else {
        app_display_set_text(
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
    }

    /* Don't wait for mount — show status immediately.
     * The USB task calls tud_mount_cb() when the host connects;
     * we rely on that background update to signal readiness. */
}

static void enter_reading_mode(void)
{
    beep();
    if (!s_card_available) {
        app_display_set_text(
            "SD card not detected.\n"
            "Please insert TF card\n"
            "and restart.");
        return;
    }

    /* USB host must disconnect before local FS access */
    if (s_usb_started && tusb_msc_is_connected()) {
        app_display_set_text(
            "USB cable still connected.\n"
            "Unplug USB from PC,\n"
            "then press KEY2 again.");
        return;
    }

    s_op_mode = MODE_READING;
    app_display_set_text(
        "KEY1: Scan files\n"
        "KEY2: Play/Pause\n"
        "KEY3: Skip ahead\n"
        "KEY4: Next file\n"
        "(hold KEY1 for menu)");
}

static void return_to_main_menu(void)
{
    beep();
    s_playing = false;
    app_tts_stop();

    /* USB stays active (don't deinit — causes PANIC).
     * Reading mode will block FATFS access if USB is connected. */
    show_main_menu();
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
            enter_usb_mode();
        } else if (key_id == 2) {
            enter_reading_mode();
        }
        break;

    case MODE_USB_COPY:
        if (key_id == 1) {
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
            if (!s_tts_available) {
                app_display_set_mode(DISP_MODE_PAUSED);
                app_display_set_text("TTS not available.\nCheck voice init/logs.");
                break;
            }
            do_toggle_play();
            break;

        case 3: /* KEY3: skip ahead */
            if (s_playing || !s_file_loaded) break;
            do_skip();
            break;

        case 4: /* KEY4: next file */
            if (!s_file_loaded) break;
            do_next_file();
            break;

        default:
            break;
        }
        break;
    }
}

static void on_button_event(int key_id, btn_event_t event)
{
    if (!s_app_event_queue) return;
    if (!s_system_ready) return;

    app_event_t msg = {
        .type = APP_EVT_BUTTON,
        .key_id = key_id,
        .button_event = event,
    };
    (void)xQueueSend(s_app_event_queue, &msg, 0);
}

static void app_event_task(void *arg)
{
    (void)arg;
    app_event_t msg;

    while (1) {
        if (xQueueReceive(s_app_event_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.type == APP_EVT_BUTTON) {
                process_button_event(msg.key_id, msg.button_event);
            } else if (msg.type == APP_EVT_TTS_DONE) {
                handle_tts_done_event();
            } else if (msg.type == APP_EVT_TTS_CHAIN_RESUME) {
                if (s_playing && !app_tts_is_busy()) {
                    ESP_LOGW(TAG, "Chain broken, re-queuing sentence %d/%d",
                             s_current_sentence, s_num_sentences - 1);
                    speak_current_sentence();
                }
            }
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
            bool busy = app_tts_is_busy();
            if (!busy) {
                idle_100ms++;
                if (idle_100ms > 50) {
                    idle_100ms = 0;
                    if (s_app_event_queue) {
                        app_event_t msg = {
                            .type = APP_EVT_TTS_CHAIN_RESUME,
                        };
                        (void)xQueueSend(s_app_event_queue, &msg, 0);
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
            ESP_LOGI(TAG, "Free internal heap: %d bytes",
                     (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#if ENABLE_HEAP_INTEGRITY_SCAN
            heap_caps_check_integrity_all(true);
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(100));
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
    ESP_LOGI(TAG, "=== Voice Reader v2.0 ===");
    nvs_flash_init();

    /* Hardware init (before LVGL — no display updates yet) */
    esp_err_t ret;
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

    /* LVGL + LCD (display now available) */
    ESP_ERROR_CHECK(app_display_init());

    /* Show init log on screen (accumulated, line by line) */
    s_init_log[0] = '\0';
    init_step("Hardware init", ESP_OK);
    init_step("SD card", s_card_available ? ESP_OK : ESP_FAIL);

    /* USB MSC (deferred — starts after user presses a key) */
    s_system_ready = false;

    /* TTS engine */
    ret = app_tts_init();
    s_tts_available = (ret == ESP_OK);
    init_step("TTS engine", ret);

    /* Buttons */
    s_app_event_queue = xQueueCreate(16, sizeof(app_event_t));
    ESP_ERROR_CHECK(s_app_event_queue ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskCreate(app_event_task, "app_evt", 8192, NULL, 2, NULL);
    ESP_ERROR_CHECK(app_buttons_init(on_button_event));
    app_buttons_scan_start();

    /* TTS polling task */
    xTaskCreate(poll_task, "tts_poll", 8192, NULL, 3, NULL);

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

    ESP_LOGI(TAG, "Ready");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
