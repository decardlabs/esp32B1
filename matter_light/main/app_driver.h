#ifndef APP_DRIVER_H
#define APP_DRIVER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_driver_set_power(bool on);
esp_err_t app_driver_identify(void);

#ifdef __cplusplus
}
#endif

#endif
