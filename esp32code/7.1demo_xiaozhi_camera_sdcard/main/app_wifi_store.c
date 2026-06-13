#include "app_wifi_store.h"
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"

#define APP_WIFI_NVS_NAMESPACE  "wifi"
#define APP_WIFI_NVS_KEY_SSID   "ssid"
#define APP_WIFI_NVS_KEY_PASS   "pass"
#define APP_WIFI_NVS_KEY_PORTAL "portal"

static const char *TAG = "APP_WIFI_STORE";

static esp_err_t app_wifi_store_load_saved(app_wifi_credentials_t *credentials)
{
    esp_err_t ret;
    nvs_handle_t nvs_handle = 0;
    size_t ssid_size = 0;
    size_t password_size = 0;

    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ssid_size = sizeof(credentials->ssid);
    password_size = sizeof(credentials->password);
    ret = nvs_get_str(nvs_handle, APP_WIFI_NVS_KEY_SSID, credentials->ssid, &ssid_size);
    if (ret == ESP_OK) {
        ret = nvs_get_str(nvs_handle, APP_WIFI_NVS_KEY_PASS, credentials->password, &password_size);
    }

    nvs_close(nvs_handle);
    if (ret == ESP_OK) {
        credentials->source = APP_WIFI_SOURCE_NVS;
    }
    return ret;
}

esp_err_t app_wifi_store_load(app_wifi_credentials_t *credentials)
{
    esp_err_t ret;

    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(credentials, 0, sizeof(*credentials));
    ret = app_wifi_store_load_saved(credentials);
    if (ret == ESP_OK) {
        return ESP_OK;
    }

    if (strlen(CONFIG_XIAOZHI_WIFI_SSID) == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(credentials->ssid, sizeof(credentials->ssid), "%s", CONFIG_XIAOZHI_WIFI_SSID);
    snprintf(credentials->password, sizeof(credentials->password), "%s", CONFIG_XIAOZHI_WIFI_PASSWORD);
    credentials->source = APP_WIFI_SOURCE_MENUCONFIG;
    return ESP_OK;
}

bool app_wifi_store_has_saved_credentials(void)
{
    app_wifi_credentials_t credentials = {0};

    return (app_wifi_store_load_saved(&credentials) == ESP_OK);
}

esp_err_t app_wifi_store_save(const char *ssid, const char *password)
{
    esp_err_t ret;
    nvs_handle_t nvs_handle = 0;

    if ((ssid == NULL) || (ssid[0] == '\0') || (strlen(ssid) > APP_WIFI_SSID_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((password != NULL) && (strlen(password) > APP_WIFI_PASSWORD_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs_handle, APP_WIFI_NVS_KEY_SSID, ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs_handle, APP_WIFI_NVS_KEY_PASS, (password != NULL) ? password : "");
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "saved wifi credentials for ssid=%s", ssid);
    }
    return ret;
}

esp_err_t app_wifi_store_clear(void)
{
    esp_err_t ret;
    nvs_handle_t nvs_handle = 0;

    ret = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_key(nvs_handle, APP_WIFI_NVS_KEY_SSID);
    if ((ret == ESP_OK) || (ret == ESP_ERR_NVS_NOT_FOUND)) {
        ret = nvs_erase_key(nvs_handle, APP_WIFI_NVS_KEY_PASS);
    }
    if ((ret == ESP_OK) || (ret == ESP_ERR_NVS_NOT_FOUND)) {
        ret = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return (ret == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : ret;
}

esp_err_t app_wifi_store_request_portal_once(void)
{
    esp_err_t ret;
    nvs_handle_t nvs_handle = 0;

    ret = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, APP_WIFI_NVS_KEY_PORTAL, 1);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return ret;
}

bool app_wifi_store_take_portal_request(void)
{
    esp_err_t ret;
    nvs_handle_t nvs_handle = 0;
    uint8_t portal_requested = 0;
    bool requested = false;

    ret = nvs_open(APP_WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        return false;
    }

    ret = nvs_get_u8(nvs_handle, APP_WIFI_NVS_KEY_PORTAL, &portal_requested);
    if ((ret == ESP_OK) && (portal_requested != 0)) {
        requested = true;
        ret = nvs_erase_key(nvs_handle, APP_WIFI_NVS_KEY_PORTAL);
        if ((ret == ESP_OK) || (ret == ESP_ERR_NVS_NOT_FOUND)) {
            (void)nvs_commit(nvs_handle);
        }
    }

    nvs_close(nvs_handle);
    return requested;
}
