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
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
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

/*
 * WAV header struct and write_wav_header() are in task_qa_lvgl.c
 * where all WAV file I/O happens (PSRAM-buffer refactor).
 */

/* ------------------------------------------------------------------ */
/*  Static state                                                      */
/* ------------------------------------------------------------------ */

static audio_capture_state_t s_state = AUDIO_IDLE;

audio_capture_state_t audio_capture_get_state(void)
{
    return s_state;
}

/* ------------------------------------------------------------------ */
/*  FreeRTOS task                                                     */
/* ------------------------------------------------------------------ */

static void audio_capture_task(void *pv_params)
{
    const config_t *cfg = (const config_t *)pv_params;
    int timeout_s = config_get_int(cfg, "AUDIO_TIMEOUT_S", 30);
    bool key3_was_pressed = false;

    ESP_LOGI(TAG, "Audio capture task started (timeout=%ds)", timeout_s);

    /* Ensure the target directory exists -- retry on each recording */
    (void)mkdir("/sdcard/AUDIO", 0755);

    while (1) {
        bool key_level;

        /* Poll KEY3 every POLL_MS */
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        if (s_state != AUDIO_IDLE) {
            continue;   /* already recording -- should not happen */
        }

        xl9555_get_pin_level(KEY_PORT, KEY3_PIN, &key_level);
        if (key_level != 0) {
            key3_was_pressed = false;
            continue;   /* not pressed (active low) */
        }

        if (key3_was_pressed) {
            continue;   /* wait for a fresh press edge */
        }
        key3_was_pressed = true;

        /* ============================================================== */
        /*  KEY3 pressed: start recording                                 */
        /* ============================================================== */
        s_state = AUDIO_RECORDING;
        qa_ui_set_status("录音中...");
        qa_ui_add_log("[MIC] 开始录音");

        /* Capture audio to PSRAM buffer, converting 24kHz stereo → 16kHz mono on-the-fly */
        #define CAPTURE_BUF_SAMPLES  480000  /* ~30 seconds of 16kHz mono output */
        int16_t *audio_buf = heap_caps_malloc(CAPTURE_BUF_SAMPLES * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (audio_buf) {
            size_t mono_count = 0;
            int empty_reads = 0;
            int64_t capture_start_us = esp_timer_get_time();

            while ((esp_timer_get_time() - capture_start_us) < ((int64_t)timeout_s * 1000000LL)) {
                /* Check KEY3 release (need ~1s minimum audio) */
                xl9555_get_pin_level(KEY_PORT, KEY3_PIN, &key_level);
                if (key_level != 0 && mono_count > 16000) break;

                /* Read I2S data (24kHz stereo interleaved L,R,L,R...) */
                static int16_t raw[1024];
                size_t raw_samples = 0;
                esp_err_t ret = bsp_audio_read(raw, 1024, &raw_samples, 100);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Audio read failed: %s", esp_err_to_name(ret));
                    break;
                }

                if (raw_samples == 0) {
                    empty_reads++;
                    if (empty_reads >= 5) {
                        ESP_LOGW(TAG, "Audio read returned 0 repeatedly");
                        break;
                    }
                    continue;
                }
                empty_reads = 0;

                /* Convert 24kHz stereo → 16kHz mono:
                 *   1. Average L+R channels
                 *   2. Downsample 3:2 (discard every 3rd frame) */
                int frames = (int)raw_samples / 2;
                for (int i = 0; i < frames && mono_count < CAPTURE_BUF_SAMPLES; i++) {
                    if (i % 3 != 2) {   /* keep 2 of every 3 frames */
                        int32_t left  = raw[i * 2];
                        int32_t right = raw[i * 2 + 1];
                        audio_buf[mono_count++] = (int16_t)((left + right) / 2);
                    }
                }
            }

            float dur = (float)mono_count / 16000.0f;
            ESP_LOGI(TAG, "Captured %.1fs (%zu mono samples)", dur, mono_count);
            qa_ui_add_log("[MIC] 录音完成 (%.1fs)", (double)dur);

            /* Check audio energy — warn if likely silence */
            {
                int64_t sum_sq = 0;
                int peak = 0;
                for (size_t i = 0; i < mono_count; i++) {
                    int32_t s = audio_buf[i];
                    sum_sq += (int64_t)s * s;
                    int abs_s = (s < 0) ? -s : s;
                    if (abs_s > peak) peak = abs_s;
                }
                int64_t rms = (mono_count > 0) ? (int64_t)sqrtf((float)(sum_sq / mono_count)) : 0;
                ESP_LOGI(TAG, "Audio energy: RMS=%lld peak=%d", (long long)rms, peak);
                if (rms < 20) {
                    ESP_LOGW(TAG, "Audio appears SILENT (RMS=%lld, peak=%d)", (long long)rms, peak);
                    qa_ui_add_log("[WARN] 音频能量过低，请靠近MIC说话");
                }
            }

            /* Delegate WAV save + ASR submit to LVGL task (safe SPI access) */
            if (mono_count > 0) {
                static unsigned int s_counter = 0;
                if (s_counter == 0) {
                    time_t now = time(NULL);
                    s_counter = (unsigned int)now;
                    if (s_counter == 0) s_counter = 1;
                }
                char wav_path[64];
                snprintf(wav_path, sizeof(wav_path), "/sdcard/AUDIO/%08u.wav", s_counter++);

                SemaphoreHandle_t save_done = xSemaphoreCreateBinary();
                if (save_done) {
                    audio_save_req_t req = {
                        .buf      = audio_buf,
                        .count    = mono_count,
                        .done     = save_done,
                    };
                    memcpy(req.wav_path, wav_path, sizeof(req.wav_path));

                    esp_err_t err = qa_ui_save_audio(&req);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "qa_ui_save_audio failed: %s", esp_err_to_name(err));
                        qa_ui_add_log("[ERR] 保存/识别失败");
                    }
                    vSemaphoreDelete(save_done);
                } else {
                    ESP_LOGE(TAG, "Failed to create save semaphore");
                }
            }

            heap_caps_free(audio_buf);
        } else {
            ESP_DRAM_LOGE(TAG, "Failed to allocate audio buffer");
            qa_ui_add_log("[ERR] 内存不足");
        }

        qa_ui_set_status("待命中 · 按住KEY3说话");
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
    return xTaskCreate(audio_capture_task, "audio_capture", 8192,
                       (void *)cfg, 5, NULL);
}
