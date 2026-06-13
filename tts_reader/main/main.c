#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdint.h>
#include <stddef.h>

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
#include "es8388.h"
#include "app_buttons.h"
#include "app_tts.h"
#include "app_display.h"

#include "esp_heap_caps.h"

static const char *TAG = "MAIN";

/* ── Constants ─────────────────────────────── */
#define MAX_FILES        64
#define MAX_FNAME_LEN   64
#define MAX_FILE_CONTENT (64 * 1024)
#define MAX_DISPLAY_TEXT 2048
#define MAX_SENTENCES    256
#define TTS_RUNTIME_MAX_BYTES 72
#define ENABLE_HEAP_INTEGRITY_SCAN 0

/* ── File management ─────────────────────── */
static char  s_file_names[MAX_FILES][MAX_FNAME_LEN];
static int   s_file_count  = 0;
static int   s_current_file = -1;
static char  s_file_content[MAX_FILE_CONTENT] = {0};
static size_t s_content_len = 0;
static bool  s_file_loaded = false;

/* Sentence splitting */
static size_t s_sentence_starts[MAX_SENTENCES];
static int    s_num_sentences = 0;
static int    s_current_sentence = 0;
static size_t s_sentence_offset = 0;
static size_t s_last_chunk_len = 0;

/* Playback state */
typedef enum {
    ST_FILE_SELECT,   /* 文件选择（上电默认）*/
    ST_READY,         /* 文件已选，等待播放 */
    ST_PLAYING,       /* 播放中 */
    ST_PAUSED,        /* 已暂停 */
} app_state_t;

static app_state_t s_state = ST_FILE_SELECT;
static bool s_playing = false;
static bool s_tts_available = false;
static bool s_card_available = false;
static bool s_system_ready = false;

/* Volume / Speed levels (0-based index, shown as 1/N) */
#define VOL_LEVELS 5
#define SPD_LEVELS 5
static const int s_vol_db[VOL_LEVELS] = {-30, -18, -12, -6, 0};
static int s_vol_idx = 2;  /* default -12 dB */
static int s_spd_idx = 2;  /* default speed=2 */

/* Button event queue */
typedef struct { int key_id; btn_event_t event; } button_msg_t;
static QueueHandle_t s_btn_q = NULL;

/* ── Text helpers (from test005, unchanged) ─── */
static size_t utf8_safe_copy(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    size_t max = (src_len < dst_size - 1) ? src_len : dst_size - 1;
    if (max < src_len) {
        while (max > 0 && ((unsigned char)src[max] & 0xC0) == 0x80) max--;
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

static size_t decode_utf16_to_utf8(const uint8_t *src, size_t src_len, bool le, char *dst, size_t dst_size)
{
    size_t out = 0;
    for (size_t i = 0; i + 1 < src_len;) {
        uint16_t w1 = le ? (uint16_t)(src[i] | (src[i+1] << 8))
                        : (uint16_t)((src[i] << 8) | src[i+1]);
        i += 2;
        if (w1 == 0x0000) continue;
        uint32_t cp = w1;
        if (w1 >= 0xD800 && w1 <= 0xDBFF && i + 1 < src_len) {
            uint16_t w2 = le ? (uint16_t)(src[i] | (src[i+1] << 8))
                            : (uint16_t)((src[i] << 8) | src[i+1]);
            if (w2 >= 0xDC00 && w2 <= 0xDFFF) {
                cp = 0x10000 + (((uint32_t)(w1 - 0xD800) << 10) | (uint32_t)(w2 - 0xDC00));
                i += 2;
            }
        }
        if (!utf8_append_cp(dst, dst_size, &out, cp)) break;
    }
    dst[out] = '\0';
    return out;
}

static size_t normalize_text_to_utf8(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
{
    if (src_len >= 3 && src[0]==0xEF && src[1]==0xBB && src[2]==0xBF) {
        size_t cp = src_len - 3; if (cp > dst_size-1) cp = dst_size-1;
        memcpy(dst, src+3, cp); dst[cp]='\0'; return cp;
    }
    if (src_len >= 2 && src[0]==0xFF && src[1]==0xFE)
        return decode_utf16_to_utf8(src+2, src_len-2, true, dst, dst_size);
    if (src_len >= 2 && src[0]==0xFE && src[1]==0xFF)
        return decode_utf16_to_utf8(src+2, src_len-2, false, dst, dst_size);
    /* Detect UTF-16 by zero frequency */
    size_t ze=0, zo=0;
    for (size_t i=0;i<src_len;i++) { if(src[i]==0){ if((i&1)==0)ze++; else zo++; } }
    if (src_len>=8 && (ze>src_len/6 || zo>src_len/6))
        return decode_utf16_to_utf8(src, src_len, ze>=zo, dst, dst_size);
    size_t cp = src_len; if (cp>dst_size-1) cp=dst_size-1;
    memcpy(dst, src, cp); dst[cp]='\0'; return cp;
}

static int split_sentences(const char *text, size_t *starts, int max_s)
{
    const size_t max_chunk_bytes = 72;
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
            /* U+3002(。) U+3001(、) */
            unsigned char b2 = (unsigned char)text[i + 2];
            if (b2 == 0x82 || b2 == 0x81) {
                i += 2;
                should_cut = true;
            }
        } else if (i + 2 < len && c == 0xEF && (unsigned char)text[i + 1] == 0xBC) {
            /* U+FF01(！) U+FF0C(，) U+FF1A(：) U+FF1B(；) U+FF1F(？) */
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
            if (cut == last_cut) {
                continue;
            }
            starts[count++] = cut;
            last_cut = cut;
        }
    }

    if (count > 0 && starts[count - 1] < len && count < max_s) starts[count++] = len;
    else if (count >= max_s) starts[max_s - 1] = len;
    else if (count == 0) { starts[0] = 0; starts[1] = len; count = 2; }
    return count;
}

static const char *get_sentence(int idx, size_t *len)
{
    if (idx<0 || idx>=s_num_sentences-1) { *len=0; return NULL; }
    *len = s_sentence_starts[idx+1] - s_sentence_starts[idx];
    return s_file_content + s_sentence_starts[idx];
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

/* ── File operations ─────────────────────────── */
static int name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char*)a, (const char*)b);
}

static bool is_temp_txt(const char *name)
{
    if (!name||!name[0]) return true;
    if (name[0]=='.'||name[0]=='~'||name[0]=='_') return true;
    size_t n=strlen(name);
    if (n>=4 && (!strcasecmp(name+n-4,".tmp")||!strcasecmp(name+n-4,".swp"))) return true;
    return false;
}

static void scan_files(void)
{
    DIR *dir = opendir("/sdcard");
    if (!dir) {
        s_file_count=0;
        s_state = ST_FILE_SELECT;
        app_display_set_mode(DISP_MODE_NO_SD);
        return;
    }
    s_file_count=0;
    struct dirent *entry;
    while ((entry=readdir(dir))!=NULL && s_file_count<MAX_FILES) {
        if (is_temp_txt(entry->d_name)) continue;
        const char *ext=strrchr(entry->d_name,'.');
        if (!ext || strcasecmp(ext,".txt")!=0) continue;
        strncpy(s_file_names[s_file_count], entry->d_name, MAX_FNAME_LEN-1);
        s_file_names[s_file_count][MAX_FNAME_LEN-1]='\0';
        s_file_count++;
    }
    closedir(dir);
    qsort(s_file_names, s_file_count, MAX_FNAME_LEN, name_cmp);
    ESP_LOGI(TAG, "Found %d .txt files", s_file_count);

    if (s_file_count==0) {
        s_state = ST_FILE_SELECT;
        app_display_set_mode(DISP_MODE_NO_FILES);
        return;
    }
    s_current_file = 0;
    s_state = ST_FILE_SELECT;
    app_display_set_file_list((const char(*)[64])s_file_names, s_file_count, s_current_file);
    app_display_set_mode(DISP_MODE_FILE_SELECT);
}

static bool load_file(int idx)
{
    if (idx<0||idx>=s_file_count) return false;
    char path[320];
    snprintf(path, sizeof(path), "/sdcard/%s", s_file_names[idx]);

    struct stat st;
    if (stat(path,&st)!=0||st.st_size==0) return false;

    size_t fsize = st.st_size;
    if (fsize > MAX_FILE_CONTENT-1) fsize = MAX_FILE_CONTENT-1;

    FILE *f = fopen(path,"rb");
    if (!f) return false;
    uint8_t *raw = malloc(MAX_FILE_CONTENT);
    if (!raw) { fclose(f); return false; }
    size_t raw_len = fread(raw,1,fsize,f);
    fclose(f);
    if (raw_len==0) { free(raw); return false; }

    s_content_len = normalize_text_to_utf8(raw, raw_len, s_file_content, sizeof(s_file_content));
    free(raw);
    if (s_content_len==0) return false;
    s_file_content[s_content_len]='\0';

    s_num_sentences = split_sentences(s_file_content, s_sentence_starts, MAX_SENTENCES);
    s_current_sentence = 0;
    s_sentence_offset = 0;
    s_last_chunk_len = 0;
    s_file_loaded = true;
    s_current_file = idx;

    /* Update display: show as much content as s_text can hold */
    app_display_set_filename(s_file_names[idx]);
    size_t plen = s_content_len > 2047 ? 2047 : s_content_len;
    char *preview = malloc(plen + 1);
    if (preview) {
        memcpy(preview, s_file_content, plen);
        preview[plen]='\0';
        app_display_set_text(preview);
        free(preview);
    }

    ESP_LOGI(TAG, "Loaded: %s (%d bytes, %d sentences)", s_file_names[idx], s_content_len, s_num_sentences);
    return true;
}

/* ── Playback ──────────────────────────────── */


/* Callback-based sentence advancement.
 * Registered as TTS completion callback so chaining runs
 * directly from the worker task — no poll race possible. */
static void on_tts_finished(void);

static void speak_current_sentence(void)
{
    if (!s_playing || !s_file_loaded || s_current_sentence >= s_num_sentences-1) return;
    size_t slen;
    const char *stext = get_sentence(s_current_sentence, &slen);
    if (!stext||slen==0) return;

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
            s_state = ST_READY;
            app_display_set_text("=== Finished ===\n\nKey1: Return\nKey4: Next");
            app_display_set_mode(DISP_MODE_READY);
            ESP_LOGI(TAG, "Playback done");
        }
        return;
    }

    size_t remaining = slen - s_sentence_offset;
    size_t chunk_len = tts_pick_chunk_len(stext + s_sentence_offset, remaining, TTS_RUNTIME_MAX_BYTES);
    if (chunk_len == 0) {
        ESP_LOGW(TAG, "empty chunk (sentence=%d, slen=%u, off=%u)",
                 s_current_sentence, (unsigned)slen, (unsigned)s_sentence_offset);
        s_playing = false;
        s_state = ST_PAUSED;
        app_display_set_mode(DISP_MODE_PAUSED);
        return;
    }

    char sentence[512];
    utf8_safe_copy(sentence, sizeof(sentence), stext + s_sentence_offset, chunk_len);
    s_last_chunk_len = chunk_len;

    esp_err_t ret = app_tts_speak_cb(sentence, on_tts_finished);
    if (ret!=ESP_OK) {
        ESP_LOGW(TAG, "speak failed: %s", esp_err_to_name(ret));
        s_playing=false;
        s_state=ST_PAUSED;
        app_display_set_mode(DISP_MODE_PAUSED);
        return;
    }
    app_display_set_progress(s_current_sentence+1, s_num_sentences-1);
}

static void on_tts_finished(void)
{
    /* Pause/stop during callback — don't advance */
    if (!s_playing) return;

    size_t slen = 0;
    (void)get_sentence(s_current_sentence, &slen);

    if (s_last_chunk_len > 0 && s_sentence_offset + s_last_chunk_len < slen) {
        s_sentence_offset += s_last_chunk_len;
    } else {
        s_sentence_offset = 0;
        s_current_sentence++;
    }

    vTaskDelay(pdMS_TO_TICKS(30));
    /* Update displayed text to current sentence */
    if (s_current_sentence < s_num_sentences-1) {
        const char *stext = get_sentence(s_current_sentence, &slen);
        if (stext&&slen>0 && s_sentence_offset == 0) {
            char buf[2048];
            const char *display_ptr = stext + s_sentence_offset;
            ptrdiff_t start_off = display_ptr - s_file_content;
            if (start_off < 0 || (size_t)start_off > s_content_len) {
                start_off = (ptrdiff_t)s_content_len;
            }
            size_t remaining = s_content_len - (size_t)start_off;
            utf8_safe_copy(buf, sizeof(buf), display_ptr, remaining);
            app_display_set_text(buf);
        }
        speak_current_sentence();
    } else {
        /* Done - show completion, KEY4=next, KEY1=return */
        s_playing=false;
        s_state=ST_READY;
        app_display_set_text("=== Finished ===\n\nKey1: Return\nKey4: Next");
        app_display_set_mode(DISP_MODE_READY);
        ESP_LOGI(TAG, "Playback done");
    }
}

/* ── Actions ───────────────────────────────── */
static void beep(void) { app_tts_play_beep(); }

static void do_select_next_file(void)
{
    if (s_file_count==0) return;
    s_current_file = (s_current_file+1) % s_file_count;
    app_display_set_file_list((const char(*)[64])s_file_names, s_file_count, s_current_file);
    ESP_LOGI(TAG, "Selected: %s", s_file_names[s_current_file]);
}

static void do_confirm_file(void)
{
    if (s_file_count==0) return;
    if (load_file(s_current_file)) {
        s_state = ST_READY;
        app_display_set_mode(DISP_MODE_READY);
        /* Set initial volume/speed */
        app_tts_set_volume(s_vol_db[s_vol_idx]);
        app_tts_set_speed(s_spd_idx);
        app_display_set_volume(s_vol_idx, VOL_LEVELS);
        app_display_set_speed(s_spd_idx, SPD_LEVELS);
    }
}

static void do_play(void)
{
    if (!s_file_loaded||s_playing) return;
    /* If playback finished, restart from beginning */
    if (s_current_sentence >= s_num_sentences-1) {
        s_current_sentence = 0;
        s_sentence_offset = 0;
        s_last_chunk_len = 0;
    }
    s_playing=true;
    s_state=ST_PLAYING;
    app_display_set_mode(DISP_MODE_PLAYING);
    speak_current_sentence();
}

static void do_pause(void)
{
    s_playing=false;
    s_last_chunk_len = 0;
    s_state=ST_PAUSED;
    app_tts_stop();
    app_display_set_mode(DISP_MODE_PAUSED);
}

static void do_vol_up(void)
{
    s_vol_idx = (s_vol_idx+1) % VOL_LEVELS;
    app_tts_set_volume(s_vol_db[s_vol_idx]);
    app_display_set_volume(s_vol_idx, VOL_LEVELS);
    ESP_LOGI(TAG, "Volume: %d dB (level %d)", s_vol_db[s_vol_idx], s_vol_idx+1);
}

static void do_spd_up(void)
{
    s_spd_idx = (s_spd_idx+1) % SPD_LEVELS;
    app_tts_set_speed(s_spd_idx);
    app_display_set_speed(s_spd_idx, SPD_LEVELS);
    ESP_LOGI(TAG, "Speed: %d (level %d)", s_spd_idx, s_spd_idx+1);
}

/* ── Button callback ────────────────────────── */
static void process_button(int key_id, btn_event_t event)
{
    if (!s_system_ready) return;
    if (event!=BTN_EVENT_SHORT_PRESS) return;

    switch (s_state) {

    case ST_FILE_SELECT:
        switch (key_id) {
        case 1: /* KEY1: cycle files */
            beep();
            do_select_next_file();
            break;
        case 2: /* KEY2: volume up */
            beep();
            do_vol_up();
            break;
        case 3: /* KEY3: speed up */
            beep();
            do_spd_up();
            break;
        case 4: /* KEY4: confirm selection */
            beep();
            do_confirm_file();
            break;
        }
        break;

    case ST_READY:
        switch (key_id) {
        case 1: /* KEY1: back to file select */
            beep();
            s_state=ST_FILE_SELECT;
            app_display_set_file_list((const char(*)[64])s_file_names, s_file_count, s_current_file);
            app_display_set_mode(DISP_MODE_FILE_SELECT);
            break;
        case 2: /* KEY2: volume */
            beep();
            do_vol_up();
            break;
        case 3: /* KEY3: speed */
            beep();
            do_spd_up();
            break;
        case 4: /* KEY4: play or next file if finished */
            beep();
            if (s_current_sentence >= s_num_sentences-1) {
                do_select_next_file();
                do_confirm_file();
            }
            do_play();
            break;
        }
        break;

    case ST_PLAYING:
        switch (key_id) {
        case 1: /* KEY1: back to file select (stop playback) */
            beep();
            s_playing=false;
            s_sentence_offset = 0;
            s_last_chunk_len = 0;
            app_tts_stop();
            s_state=ST_FILE_SELECT;
            app_display_set_file_list((const char(*)[64])s_file_names, s_file_count, s_current_file);
            app_display_set_mode(DISP_MODE_FILE_SELECT);
            break;
        case 2: /* KEY2: volume */
            beep();
            do_vol_up();
            break;
        case 3: /* KEY3: speed */
            beep();
            do_spd_up();
            break;
        case 4: /* KEY4: pause */
            beep();
            do_pause();
            break;
        }
        break;

    case ST_PAUSED:
        switch (key_id) {
        case 1: /* KEY1: back to file select */
            beep();
            s_state=ST_FILE_SELECT;
            app_display_set_file_list((const char(*)[64])s_file_names, s_file_count, s_current_file);
            app_display_set_mode(DISP_MODE_FILE_SELECT);
            break;
        case 2: /* KEY2: volume */
            beep();
            do_vol_up();
            break;
        case 3: /* KEY3: speed */
            beep();
            do_spd_up();
            break;
        case 4: /* KEY4: resume */
            beep();
            do_play();  /* resumes from current sentence */
            break;
        }
        break;
    }
}

static void button_cb(int key_id, btn_event_t event)
{
    if (!s_btn_q) return;
    button_msg_t msg = {.key_id=key_id, .event=event};
    xQueueSend(s_btn_q, &msg, 0);
}

static void button_task(void *arg)
{
    (void)arg;
    button_msg_t msg;
    while (1) {
        if (xQueueReceive(s_btn_q, &msg, portMAX_DELAY)==pdTRUE)
            process_button(msg.key_id, msg.event);
    }
}

/* ── TTS poll task ─────────────────────────── */
/* Only serves as safety net — normal advancement is callback-driven. */
static void poll_task(void *arg)
{
    (void)arg;
    int idle_100ms = 0;
    int tick = 0;
    while (1) {
        bool busy = app_tts_is_busy();

        /* If playback is active but TTS idle too long, the callback
         * chain likely broke — re-queue current sentence. */
        if (s_playing && !busy) {
            idle_100ms++;
            if (idle_100ms > 50) {  /* 5 seconds */
                idle_100ms = 0;
                ESP_LOGW(TAG, "Chain broken after 5s idle, re-queuing");
                speak_current_sentence();
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

/* ── Init ──────────────────────────────────── */
void app_main(void)
{
    esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %d", (int)rr);
    nvs_flash_init();

    /* Hardware */
    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(xl9555_init());
    ESP_ERROR_CHECK(bsp_spi2_lcd_init());

    /* SD card */
    sdmmc_card_t *card = NULL;
    esp_err_t ret = sd_card_init(&card);
    s_card_available = (ret==ESP_OK);
    if (s_card_available) ESP_LOGI(TAG, "SD card mounted");
    else ESP_LOGW(TAG, "SD card not available");

    /* Display */
    ESP_ERROR_CHECK(app_display_init());
    app_display_set_mode(DISP_MODE_FILE_SELECT);
    app_display_set_text("Initializing...");

    /* TTS engine */
    ret = app_tts_init();
    s_tts_available = (ret==ESP_OK);
    if (ret==ESP_OK) ESP_LOGI(TAG, "TTS ready");
    else ESP_LOGW(TAG, "TTS init failed: %s", esp_err_to_name(ret));

    /* Set default volume/speed */
    app_tts_set_volume(s_vol_db[s_vol_idx]);
    app_tts_set_speed(s_spd_idx);

    /* Buttons */
    s_btn_q = xQueueCreate(16, sizeof(button_msg_t));
    ESP_ERROR_CHECK(s_btn_q?ESP_OK:ESP_ERR_NO_MEM);
    xTaskCreate(button_task, "btn_evt", 8192, NULL, 2, NULL);
    ESP_ERROR_CHECK(app_buttons_init(button_cb));
    app_buttons_scan_start();

    /* TTS poll task */
    xTaskCreate(poll_task, "tts_poll", 8192, NULL, 3, NULL);

    /* Initial file scan (if card present) */
    if (s_card_available) {
        scan_files();
    } else {
        app_display_set_mode(DISP_MODE_NO_SD);
    }

    s_system_ready = true;
    ESP_LOGI(TAG, "System ready — state: FILE_SELECT");

    /* Main loop (keep alive) */
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
