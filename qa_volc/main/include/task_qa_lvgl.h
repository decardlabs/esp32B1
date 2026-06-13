#ifndef TASK_QA_LVGL_H
#define TASK_QA_LVGL_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t lcd_lvgl_reserve_buffer(void);
BaseType_t lcd_lvgl_task_create(void);

void qa_ui_add_user_msg(const char *text);
void qa_ui_add_assistant_msg(const char *text);
void qa_ui_add_log(const char *fmt, ...);
void qa_ui_set_status(const char *text);
void qa_ui_clear_all(void);

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

#endif
