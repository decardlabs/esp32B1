#ifndef __TASK_LCD_LVGL_H__
#define __TASK_LCD_LVGL_H__

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t lcd_lvgl_reserve_buffer(void);
BaseType_t lcd_lvgl_task_create(void);

#endif
