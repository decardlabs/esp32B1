#include "app_wifi.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"

static const char *TAG = "WIFI";
static bool s_connected = false;
static wifi_cb_t s_callback = NULL;
static bool s_init_done = false;

/* Pending config — stored by app_wifi_connect, applied on STA_START event */
static wifi_config_t s_pending_cfg;
static bool s_pending = false;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_pending) {
            s_pending = false;
            esp_wifi_set_config(WIFI_IF_STA, &s_pending_cfg);
            ESP_LOGI(TAG, "Connecting to SSID: %s", (char *)s_pending_cfg.sta.ssid);
        }
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        s_connected = false;
        if (s_callback) s_callback(false);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        if (s_callback) s_callback(true);
    }
}

esp_err_t app_wifi_init(void)
{
    if (s_init_done) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_init_done = true;
    ESP_LOGI(TAG, "WiFi init done");
    return ESP_OK;
}

esp_err_t app_wifi_connect(const char *ssid, const char *pass)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    memset(&s_pending_cfg, 0, sizeof(s_pending_cfg));
    s_pending_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    s_pending_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    strncpy((char *)s_pending_cfg.sta.ssid, ssid, sizeof(s_pending_cfg.sta.ssid) - 1);
    if (pass && strlen(pass) > 0) {
        strncpy((char *)s_pending_cfg.sta.password, pass, sizeof(s_pending_cfg.sta.password) - 1);
    }
    s_pending = true;

    /* Defer to STA_START handler so WiFi driver is in correct state */
    ESP_LOGI(TAG, "Config stored for SSID: %s (connect on STA_START)", ssid);
    return ESP_OK;
}

void app_wifi_disconnect(void)
{
    esp_wifi_disconnect();
    s_connected = false;
}

bool app_wifi_is_connected(void)
{
    return s_connected;
}

void app_wifi_set_callback(wifi_cb_t cb)
{
    s_callback = cb;
}
