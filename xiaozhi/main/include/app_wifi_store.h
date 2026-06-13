#ifndef __APP_WIFI_STORE_H__
#define __APP_WIFI_STORE_H__

#include <stdbool.h>
#include "esp_err.h"

#define APP_WIFI_SSID_MAX_LEN      32
#define APP_WIFI_PASSWORD_MAX_LEN  64

typedef enum {
    APP_WIFI_SOURCE_NONE = 0,
    APP_WIFI_SOURCE_NVS,
    APP_WIFI_SOURCE_MENUCONFIG,
} app_wifi_source_t;

typedef struct {
    char ssid[APP_WIFI_SSID_MAX_LEN + 1];
    char password[APP_WIFI_PASSWORD_MAX_LEN + 1];
    app_wifi_source_t source;
} app_wifi_credentials_t;

esp_err_t app_wifi_store_load(app_wifi_credentials_t *credentials);
bool app_wifi_store_has_saved_credentials(void);
esp_err_t app_wifi_store_save(const char *ssid, const char *password);
esp_err_t app_wifi_store_clear(void);
esp_err_t app_wifi_store_request_portal_once(void);
bool app_wifi_store_take_portal_request(void);

#endif
