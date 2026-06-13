#ifndef TASK_LCD_LVGL_H
#define TASK_LCD_LVGL_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

BaseType_t lcd_lvgl_task_create(void);

void lcd_lvgl_update_status(const char *status);
void lcd_lvgl_show_pairing_info(const char *qr_code_str, const char *manual_code);

#endif
