#ifndef __BSP_WIFI_H__
#define __BSP_WIFI_H__

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t bsp_wifi_sta_connect(const char *ssid, const char *password, TickType_t wait_ticks);
bool bsp_wifi_sta_is_connected(void);

#endif
