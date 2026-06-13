/*
 * task_audio_capture.c - KEY3-triggered I2S audio capture to WAV file
 *
 * Press KEY3 to start recording; release or 30s timeout to stop.
 * Captured PCM is saved as 16 kHz, 16-bit, mono WAV on the SD card.
 *
 * Audio path: ES8388 (I2S) -> ESP32 I2S RX -> stereo-to-mono + 24k->16k
 * downsampling -> WAV file on /sdcard/AUDIO/
 */

#include "task_audio_capture.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "task_qa_lvgl.h"
#include "xl9555.h"

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

static const char *TAG = "AUDIO_CAPTURE";

/* I2S is configured in bsp_audio.c for 24 kHz stereo 16-bit.
 * We read in small batches, convert to 16 kHz mono, and write to WAV. */
#define I2S_BUF_SAMPLES     512     /* number of int16_t values per I2S read  */
#define MONO_BUF_MAX        (I2S_BUF_SAMPLES / 2)  /* max mono samples       */

/* Repeat interval for the idle-loop key scan */
#define POLL_MS             50

/* Debounce delay after recording stops before re-scanning the key */
#define DEBOUNCE_MS         250

/* ------------------------------------------------------------------ */
/*  WAV header (RIFF / PCM)                                           */
/* ------------------------------------------------------------------ */

typedef struct __attribute__((packed)) {
    char     riff[4];        /* "RIFF"                                    */
    uint32_t file_size;      /* total file size - 8 (i.e. 36 + data_size) */
    char     wave[4];        /* "WAVE"                                    */
    char     fmt[4];         /* "fmt "                                    */
    uint32_t fmt_size;       /* size of the fmt chunk (16 for PCM)        */
    uint16_t format;         /* 1 = PCM                                   */
    uint16_t channels;       /* 1 = mono                                  */
    uint32_t sample_rate;    /* 16000                                     */
    uint32_t byte_rate;      /* sample_rate * channels * bits_per_sample/8 */
    uint16_t block_align;    /* channels * bits_per_sample/8              */
    uint16_t bits_per_sample;/* 16                                        */
    char     data[4];        /* "data"                                    */
    uint32_t data_size;      /* bytes of PCM data                         */
} wav_header_t;

/* ------------------------------------------------------------------ */
/*  Static state                                                      */
/* ------------------------------------------------------------------ */

static audio_capture_state_t s_state = AUDIO_IDLE;
static char s_last_file[64] = { 0 };

/* ------------------------------------------------------------------ */
/*  Public state accessors                                            */
/* ------------------------------------------------------------------ */

audio_capture_state_t audio_capture_get_state(void)
{
    return s_state;
}

const char *audio_capture_get_last_file(void)
{
    return s_last_file[0] ? s_last_file : NULL;
}

/* ------------------------------------------------------------------ */
/*  WAV helpers                                                       */
/* ------------------------------------------------------------------ */

/** Write a complete WAV header at the current file position. */
static void write_wav_header(FILE *f, uint32_t data_size)
{
    wav_header_t hdr = {
        .riff            = { 'R', 'I', 'F', 'F' },
        .file_size       = 36 + data_size,
        .wave            = { 'W', 'A', 'V', 'E' },
        .fmt             = { 'f', 'm', 't', ' ' },
        .fmt_size        = 16,
        .format          = 1,
        .channels        = 1,
        .sample_rate     = 16000,
        .byte_rate       = 16000 * 1 * 16 / 8,   /* 32000 */
        .block_align     = 1 * 16 / 8,            /* 2     */
        .bits_per_sample = 16,
        .data            = { 'd', 'a', 't', 'a' },
        .data_size       = data_size,
    };
    fwrite(&hdr, sizeof(hdr), 1, f);
}

/** Generate a timestamp-based WAV filename into `buf` (size at least 48). */
static void make_filename(char *buf, size_t buf_size)
{
    time_t now = time(NULL);
    struct tm tm_info;

    /* localtime_r is thread-safe; on failure fall back to a counter-based name */
    if (localtime_r(&now, &tm_info) == NULL) {
        /* If the RTC has not been set, localtime may fail.  Use epoch time as
         * a numeric suffix so the name is still (probably) unique. */
        snprintf(buf, buf_size, "/sdcard/AUDIO/%u.wav", (unsigned)now);
        return;
    }

    strftime(buf, buf_size, "/sdcard/AUDIO/%Y%m%d_%H%M%S.wav", &tm_info);
}

/* ------------------------------------------------------------------ */
/*  Audio capture logic                                               */
/* ------------------------------------------------------------------ */

/**
 * Run the recording sub-loop:
 *  - Read I2S data
 *  - Convert 24 kHz stereo to 16 kHz mono
 *  - Write to WAV file
 *  - Stop when KEY3 is released or timeout expires
 *
 * \param[in]  f          Open FILE pointer (writable, positioned after header)
 * \param[in]  timeout_s  Maximum recording duration in seconds
 * \return total number of PCM data bytes written (excluding the 44-byte header)
 */
static uint32_t record_loop(FILE *f, int timeout_s)
{
    /*
     * I2S delivers 24 kHz stereo interleaved (L,R,L,R,...).
     * We convert to 16 kHz mono in two steps:
     *   1. Stereo -> mono: average left and right channels.
     *   2. Downsample 24 kHz -> 16 kHz: keep 2 out of every 3 samples.
     */
    int16_t i2s_buf[I2S_BUF_SAMPLES];
    int16_t mono_buf[MONO_BUF_MAX];

    uint32_t data_bytes = 0;
    int64_t start_us = esp_timer_get_time();
    bool stop = false;

    while (!stop) {
        size_t samples_read = 0;   /* number of int16_t values returned */

        esp_err_t ret = bsp_audio_read(i2s_buf, I2S_BUF_SAMPLES,
                                       &samples_read, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
            break;
        }

        /*
         * Convert valid stereo frames to mono, applying 24k->16k decimation.
         *
         * samples_read is the number of int16_t values returned.  Normally it
         * equals I2S_BUF_SAMPLES, but we guard against partial reads.
         */
        int stereo_frames = (int)samples_read / 2;   /* each frame = L+R   */
        int mono_count = 0;

        for (int i = 0; i < stereo_frames; i++) {
            /*
             * Downsample: 24 000 -> 16 000  (ratio 2:3).
             * Discard the third sample of every triplet.
             */
            if (i % 3 != 2) {
                int32_t left  = i2s_buf[i * 2];
                int32_t right = i2s_buf[i * 2 + 1];
                mono_buf[mono_count++] = (int16_t)((left + right) / 2);
            }
        }

        if (mono_count > 0) {
            size_t written = fwrite(mono_buf, sizeof(int16_t), mono_count, f);
            if (written != (size_t)mono_count) {
                ESP_LOGE(TAG, "File write failed (disk full?) -- stopping");
                break;
            }
            data_bytes += (uint32_t)(written * sizeof(int16_t));
        }

        /* --- Check stop conditions --- */

        /* KEY3 released (active low; readback == 1 means released) */
        bool key_level;
        xl9555_get_pin_level(KEY_PORT, KEY3_PIN, &key_level);
        if (key_level != 0) {
            stop = true;
        }

        /* Timeout */
        int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (elapsed_us >= (int64_t)timeout_s * 1000000LL) {
            ESP_LOGI(TAG, "Recording timeout (%d s)", timeout_s);
            stop = true;
        }
    }

    return data_bytes;
}

/* ------------------------------------------------------------------ */
/*  FreeRTOS task                                                     */
/* ------------------------------------------------------------------ */

static void audio_capture_task(void *pv_params)
{
    const config_t *cfg = (const config_t *)pv_params;
    int timeout_s = config_get_int(cfg, "AUDIO_TIMEOUT_S", 30);

    ESP_LOGI(TAG, "Audio capture task started (timeout=%ds)", timeout_s);

    /* Ensure the target directory exists -- harmless if it already does. */
    mkdir("/sdcard/AUDIO", 0755);

    while (1) {
        bool key_level;

        /* Poll KEY3 every POLL_MS */
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        if (s_state != AUDIO_IDLE) {
            continue;   /* already recording -- should not happen */
        }

        xl9555_get_pin_level(KEY_PORT, KEY3_PIN, &key_level);
        if (key_level != 0) {
            continue;   /* not pressed (active low) */
        }

        /* ============================================================== */
        /*  KEY3 pressed: start recording                                 */
        /* ============================================================== */
        s_state = AUDIO_RECORDING;
        qa_ui_set_status("录音中...");
        qa_ui_add_log("[MIC] 开始录音");

        /* Generate a unique filename */
        char filepath[64];
        make_filename(filepath, sizeof(filepath));
        strncpy(s_last_file, filepath, sizeof(s_last_file) - 1);
        s_last_file[sizeof(s_last_file) - 1] = '\0';

        ESP_LOGI(TAG, "Recording to %s", filepath);

        FILE *f = fopen(filepath, "wb");
        if (f == NULL) {
            ESP_LOGE(TAG, "Cannot open %s for writing", filepath);
            qa_ui_add_log("[MIC] 创建文件失败");
            qa_ui_set_status("按住KEY3说话");
            s_state = AUDIO_IDLE;
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            continue;
        }

        /* Write placeholder header (data_size = 0) */
        write_wav_header(f, 0);

        /* Record until KEY3 release / timeout */
        uint32_t data_size = record_loop(f, timeout_s);

        /* Finalise the WAV header with the actual data size */
        if (fseek(f, 0, SEEK_SET) == 0) {
            write_wav_header(f, data_size);
        } else {
            ESP_LOGW(TAG, "fseek failed -- WAV header may be incomplete");
        }

        fclose(f);

        /* Report completion */
        int total_sec = (data_size / 2) / 16000;   /* rough, ok for UI */
        ESP_LOGI(TAG, "Recording saved: %s (%u PCM bytes, ~%d s)",
                 filepath, data_size, total_sec);
        qa_ui_add_log("[MIC] 录音完成 (%ds)", total_sec);
        qa_ui_set_status("按住KEY3说话");

        s_state = AUDIO_IDLE;

        /* Debounce before accepting the next press */
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

BaseType_t audio_capture_task_create(const config_t *cfg)
{
    return xTaskCreate(audio_capture_task, "audio_capture", 4096,
                       (void *)cfg, 5, NULL);
}
