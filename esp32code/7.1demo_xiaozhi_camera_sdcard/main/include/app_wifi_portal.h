#ifndef __APP_WIFI_PORTAL_H__
#define __APP_WIFI_PORTAL_H__

#include <stdbool.h>
#include "esp_err.h"

esp_err_t app_wifi_portal_start(void);
bool app_wifi_portal_is_running(void);
const char *app_wifi_portal_get_ap_ssid(void);
const char *app_wifi_portal_get_ap_ip(void);

#endif
