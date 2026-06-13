#ifndef __LCD_ST7796_H__
#define __LCD_ST7796_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#define LCD_ST7796_H_RES       320
#define LCD_ST7796_V_RES       480
#define LCD_ST7796_BUF_LINES   40

typedef enum {
    LCD_ST7796_TRANSFER_NONE = 0,
    LCD_ST7796_TRANSFER_LVGL,
} lcd_st7796_transfer_t;

esp_err_t lcd_st7796_init(void);
esp_err_t lcd_st7796_register_event_callbacks(const esp_lcd_panel_io_callbacks_t *callbacks, void *user_ctx);
esp_err_t lcd_st7796_draw_bitmap(uint16_t x_start,
                                 uint16_t y_start,
                                 uint16_t x_end,
                                 uint16_t y_end,
                                 const void *color_data);
esp_err_t lcd_st7796_draw_bitmap_owned(uint16_t x_start,
                                       uint16_t y_start,
                                       uint16_t x_end,
                                       uint16_t y_end,
                                       const void *color_data,
                                       lcd_st7796_transfer_t transfer_type);
lcd_st7796_transfer_t lcd_st7796_take_completed_transfer_from_isr(void);
esp_err_t lcd_st7796_backlight_on(void);
esp_err_t lcd_st7796_backlight_off(void);
bool lcd_st7796_is_initialized(void);

#endif
