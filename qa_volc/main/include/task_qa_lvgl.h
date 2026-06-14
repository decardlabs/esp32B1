#ifndef TASK_QA_LVGL_H
#define TASK_QA_LVGL_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

esp_err_t lcd_lvgl_reserve_buffer(void);
BaseType_t lcd_lvgl_task_create(void);

void qa_ui_add_user_msg(const char *text);
void qa_ui_add_assistant_msg(const char *text);
void qa_ui_add_log(const char *fmt, ...);
void qa_ui_set_status(const char *text);
void qa_ui_clear_all(void);

/** Scroll chat view by direction (+1 = down, -1 = up) */
void qa_ui_scroll(int direction);

/** 内存降级级别 */
typedef enum {
    QA_DEGRADE_NONE = 0,
    QA_DEGRADE_LVGL_BUF_HALF,
    QA_DEGRADE_NO_ANIM,
    QA_DEGRADE_MINIMAL,
} qa_degrade_level_t;

qa_degrade_level_t qa_degrade_get_level(void);
esp_err_t qa_degrade_step_up(void);
void qa_degrade_reset(void);

/** 音频保存请求（音频任务 → LVGL 任务，避免 SPI 冲突） */
typedef struct {
    int16_t *buf;            /* 16kHz 16-bit mono PCM samples (in PSRAM) */
    size_t count;            /* number of samples */
    char wav_path[64];       /* destination path on SD card */
    SemaphoreHandle_t done;  /* binary semaphore, given after save completes */
} audio_save_req_t;

/**
 * @brief Save captured audio to SD card and submit to ASR pipeline.
 *
 * Must be called from the audio capture task.  The actual file I/O runs
 * inside the LVGL task (which has safe access to the shared SPI2 bus).
 * Blocks until the LVGL task completes or a timeout expires.
 *
 * @param req  Request context (buffer owned by caller until this returns).
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if LVGL didn't respond.
 */
esp_err_t qa_ui_save_audio(audio_save_req_t *req);

#endif
