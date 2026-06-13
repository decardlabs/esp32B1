#ifndef APP_WIFI_H
#define APP_WIFI_H

#include "esp_err.h"
#include <stdbool.h>

typedef void (*wifi_cb_t)(bool connected);

esp_err_t app_wifi_init(void);
esp_err_t app_wifi_connect(const char *ssid, const char *pass);
void app_wifi_disconnect(void);
bool app_wifi_is_connected(void);
void app_wifi_set_callback(wifi_cb_t cb);

#endif
