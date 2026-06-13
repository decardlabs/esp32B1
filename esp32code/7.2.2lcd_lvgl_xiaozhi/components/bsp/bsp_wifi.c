#include "bsp_wifi.h"
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"

#define BSP_WIFI_CONNECTED_BIT  BIT0
#define BSP_WIFI_FAIL_BIT       BIT1
#define BSP_WIFI_MAX_RETRY      5

static const char *TAG = "BSP_WIFI";

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_wifi_netif = NULL;
static bool s_wifi_started = false;
static bool s_wifi_connected = false;
static int s_wifi_retry_count = 0;

static void bsp_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_START)) {
        esp_wifi_connect();
    } else if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        s_wifi_connected = false;
        if (s_wifi_retry_count < BSP_WIFI_MAX_RETRY) {
            s_wifi_retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "retry wifi connect %d/%d", s_wifi_retry_count, BSP_WIFI_MAX_RETRY);
        } else if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, BSP_WIFI_FAIL_BIT);
        }
    } else if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        s_wifi_retry_count = 0;
        s_wifi_connected = true;
        ESP_LOGI(TAG, "wifi got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, BSP_WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t bsp_wifi_sta_connect(const char *ssid, const char *password, TickType_t wait_ticks)
{
    esp_err_t ret;

    ESP_RETURN_ON_FALSE((ssid != NULL) && (ssid[0] != '\0'), ESP_ERR_INVALID_ARG, TAG, "ssid is empty");

    if (s_wifi_connected) {
        return ESP_OK;
    }

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create wifi event group failed");
    }

    ret = esp_netif_init();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        return ret;
    }

    if (s_wifi_netif == NULL) {
        s_wifi_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_wifi_netif != NULL, ESP_FAIL, TAG, "create wifi netif failed");
    }

    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, bsp_wifi_event_handler, NULL),
                            TAG, "register wifi event failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, bsp_wifi_event_handler, NULL),
                            TAG, "register ip event failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi set mode failed");
        s_wifi_started = true;
    }

    wifi_config_t wifi_config = {0};

    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password != NULL) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    xEventGroupClearBits(s_wifi_event_group, BSP_WIFI_CONNECTED_BIT | BSP_WIFI_FAIL_BIT);
    s_wifi_retry_count = 0;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "wifi set config failed");
    ret = esp_wifi_start();
    if ((ret != ESP_OK) && (ret != ESP_ERR_WIFI_CONN)) {
        ESP_RETURN_ON_ERROR(ret, TAG, "wifi start failed");
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           BSP_WIFI_CONNECTED_BIT | BSP_WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           wait_ticks);

    if ((bits & BSP_WIFI_CONNECTED_BIT) != 0) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "wifi connect failed");
    return ESP_ERR_TIMEOUT;
}

bool bsp_wifi_sta_is_connected(void)
{
    return s_wifi_connected;
}
