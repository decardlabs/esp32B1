#ifndef __TASK_LCD_LVGL_CAMERA_H__
#define __TASK_LCD_LVGL_CAMERA_H__

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

typedef struct {
    lv_obj_t *note_label;
    SemaphoreHandle_t io_done_sem;
    lv_color_t *fallback_buf;
    int fallback_buf_lines;
} lcd_lvgl_camera_bindings_t;

void lcd_lvgl_camera_init_runtime(void);
esp_err_t lcd_lvgl_camera_reserve_buffers(void);
void lcd_lvgl_camera_set_bindings(const lcd_lvgl_camera_bindings_t *bindings);
esp_err_t lcd_lvgl_camera_enter(void);
void lcd_lvgl_camera_leave(void);
void lcd_lvgl_camera_poll_key(bool camera_view_active);
void lcd_lvgl_camera_service_beep(void);
bool lcd_lvgl_camera_take_overlay_pending(void);
void lcd_lvgl_camera_render_frame(void);

#endif
