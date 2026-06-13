#ifndef __XL9555_H__
#define __XL9555_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define XL9555_I2C_ADDR  0x20

typedef enum {
    XL9555_BOARD_LED1 = 1,
    XL9555_BOARD_LED2,
    XL9555_BOARD_LED3,
    XL9555_BOARD_LED4,
} xl9555_board_led_t;

typedef enum {
    XL9555_KEY1 = 1,
    XL9555_KEY2,
    XL9555_KEY3,
    XL9555_KEY4,
} xl9555_key_t;

esp_err_t xl9555_init(void);
bool xl9555_is_initialized(void);

esp_err_t xl9555_lcd_gpio_init(void);
esp_err_t xl9555_lcd_reset(void);
esp_err_t xl9555_lcd_backlight_on(void);
esp_err_t xl9555_lcd_backlight_off(void);

esp_err_t xl9555_ws2812_level_shifter_enable(void);

esp_err_t xl9555_board_led_set(xl9555_board_led_t led, bool on);
esp_err_t xl9555_keys_gpio_init(void);
esp_err_t xl9555_key_is_pressed(xl9555_key_t key, bool *pressed);
esp_err_t xl9555_beep_set(bool on);

esp_err_t xl9555_camera_gpio_init(void);
esp_err_t xl9555_audio_gpio_init(void);

#endif
