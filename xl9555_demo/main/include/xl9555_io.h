#ifndef XL9555_IO_H
#define XL9555_IO_H

#include <stdbool.h>
#include "esp_err.h"
#include "io_map.h"

esp_err_t xl9555_io_init(void);
esp_err_t xl9555_io_key_is_pressed(const io_map_t *key, bool *pressed);
esp_err_t xl9555_io_led_set(const io_map_t *led, bool on);
esp_err_t xl9555_io_beep_set(const io_map_t *beep_io, bool on);

#endif /* XL9555_IO_H */
