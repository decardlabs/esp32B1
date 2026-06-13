#ifndef __XPT2046_H__
#define __XPT2046_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t xpt2046_init(void);
bool xpt2046_is_initialized(void);
esp_err_t xpt2046_read_point(uint16_t screen_w,
                             uint16_t screen_h,
                             uint16_t *x,
                             uint16_t *y,
                             bool *pressed);
esp_err_t xpt2046_get_last_sample(uint16_t *z1,
                                  uint16_t *raw_x,
                                  uint16_t *raw_y,
                                  bool *touched);
esp_err_t xpt2046_get_debug_info(int *spi_mode);
esp_err_t xpt2046_set_spi_mode(int spi_mode);

#ifdef __cplusplus
}
#endif

#endif
