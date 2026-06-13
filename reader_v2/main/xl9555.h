#ifndef XL9555_H
#define XL9555_H

#include <stdbool.h>
#include "esp_err.h"

/* Key identifiers */
typedef enum {
    XL9555_KEY1 = 1,
    XL9555_KEY2,
    XL9555_KEY3,
    XL9555_KEY4,
} xl9555_key_t;

esp_err_t xl9555_init(void);
esp_err_t xl9555_lcd_control_init(void);
esp_err_t xl9555_lcd_reset(void);
esp_err_t xl9555_backlight_on(void);
esp_err_t xl9555_backlight_off(void);
esp_err_t xl9555_speaker_enable(bool on);
esp_err_t xl9555_beep_enable(bool on);
esp_err_t xl9555_key_init(void);
esp_err_t xl9555_key_read(xl9555_key_t key, bool *pressed);
const char *xl9555_key_name(xl9555_key_t key);

#endif
