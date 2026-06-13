#include "app_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

/* ---------- constants -------------------------------------------------- */

static const char *TAG = "app_wifi";

/**
 * Maximum retry attempts before giving up automatic reconnection.
 * After exhausting retries the module stays disconnected until the
 * user calls app_wifi_start() again.
 */
#define WIFI_MAX_RETRY 5

/* ---------- static state ----------------------------------------------- */

static bool          s_inited     = false;
static bool          s_connected  = false;
static char          s_ip[16]     = "0.0.0.0";
static int           s_retry_cnt  = 0;
static app_wifi_cb_t s_cb         = NULL;

/* ---------- forward declarations --------------------------------------- */

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data);

/* ---------- public API ------------------------------------------------- */

esp_err_t app_wifi_start(const char *ssid, const char *password)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID is NULL or empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_inited) {
        ESP_LOGW(TAG, "Wi-Fi already initialised, ignoring duplicate start");
        return ESP_FAIL;
    }

    /* ----- one-time ESP-NETIF / event-loop init ----- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* ----- Wi-Fi driver init ----- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* ----- register event handlers ----- */
    esp_event_handler_instance_t handler_any;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &handler_any));
    esp_event_handler_instance_t handler_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &handler_ip));

    /* ----- station config ----- */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)wifi_config.sta.password, password,
                sizeof(wifi_config.sta.password) - 1);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /* ----- start Wi-Fi; connection is triggered by STA_START event ----- */
    ESP_ERROR_CHECK(esp_wifi_start());

    s_inited = true;
    s_retry_cnt = 0;

    ESP_LOGI(TAG, "Starting Wi-Fi STA, connecting to '%s'", ssid);
    return ESP_OK;
}

void app_wifi_register_cb(app_wifi_cb_t cb)
{
    s_cb = cb;
}

const char *app_wifi_get_ip(void)
{
    return s_ip;
}

bool app_wifi_is_connected(void)
{
    return s_connected;
}

/* ---------- internal: event handler ------------------------------------ */

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting...");
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) {
            /* We *were* connected -- notify disconnection */
            s_connected = false;
            s_ip[0] = '\0';
            ESP_LOGI(TAG, "Disconnected from AP");
            if (s_cb) {
                s_cb(false, "");
            }
        }

        /* Auto-reconnect with retry limit */
        if (s_retry_cnt < WIFI_MAX_RETRY) {
            s_retry_cnt++;
            ESP_LOGI(TAG, "Reconnecting... (attempt %d/%d)",
                     s_retry_cnt, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "Max retries reached, giving up");
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_ip, sizeof(s_ip));

        s_connected = true;
        s_retry_cnt = 0;

        ESP_LOGI(TAG, "Got IP: %s", s_ip);

        if (s_cb) {
            s_cb(true, s_ip);
        }
    }
}
