#ifndef __TASK_LCD_LVGL_H__
#define __TASK_LCD_LVGL_H__

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t lcd_lvgl_reserve_buffer(void);
void lcd_lvgl_set_status(const char *text);
void lcd_lvgl_set_record_text(const char *text);
void lcd_lvgl_set_reply_text(const char *text);
BaseType_t lcd_lvgl_task_create(void);

#endif
