#include "wifi_mqtt.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WIFI_MQTT";

static EventGroupHandle_t s_wifi_event = NULL;
#define WIFI_CONNECTED_BIT  BIT0
#define MQTT_CONNECTED_BIT  BIT1

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static mqtt_cmd_cb_t s_on_cmd = NULL;
static bool s_mqtt_started = false;

static char s_ip_str[16] = "";
static int s_rssi = 0;
static char s_topic_cmd[64];
static char s_topic_status[64];
static char s_topic_heartbeat[64];
static char s_mqtt_uri[128];

/* ── MQTT start/stop ────────────────────────────────────────────────── */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data);
static void start_mqtt(void);
static void stop_mqtt(void);

/* ── MQTT start/stop ────────────────────────────────────────────────── */

static void start_mqtt(void)
{
    if (s_mqtt_started) return;

    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtt://%s:%d", MQTT_BROKER, MQTT_PORT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_mqtt_uri,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.last_will.topic = s_topic_status,
        .session.last_will.msg = "{\"online\":false}",
        .session.last_will.msg_len = 17,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_mqtt_client) {
        ESP_LOGE(TAG, "MQTT client init failed");
        return;
    }

    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
    s_mqtt_started = true;
    ESP_LOGI(TAG, "MQTT client started, connecting to %s", s_mqtt_uri);
}

static void stop_mqtt(void)
{
    if (!s_mqtt_started || !s_mqtt_client) return;
    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    s_mqtt_started = false;
    xEventGroupClearBits(s_wifi_event, MQTT_CONNECTED_BIT);
    ESP_LOGI(TAG, "MQTT client stopped");
}

/* ── WiFi event handlers ─────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "WiFi connecting...");
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        int reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", reason);
        xEventGroupClearBits(s_wifi_event, WIFI_CONNECTED_BIT);
        stop_mqtt();
        esp_wifi_connect();
        break;
    }
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&evt->ip_info.ip, s_ip_str, sizeof(s_ip_str));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        xEventGroupSetBits(s_wifi_event, WIFI_CONNECTED_BIT);
        start_mqtt();
    }
}

/* ── MQTT event handler ──────────────────────────────────────────────── */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    esp_mqtt_event_handle_t evt = (esp_mqtt_event_handle_t)data;

    switch (evt->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd, 1);
        ESP_LOGI(TAG, "Subscribed: %s", s_topic_cmd);
        xEventGroupSetBits(s_wifi_event, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        xEventGroupClearBits(s_wifi_event, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DATA:
        if (s_on_cmd && evt->topic_len > 0) {
            if (evt->topic_len == strlen(s_topic_cmd) &&
                memcmp(evt->topic, s_topic_cmd, evt->topic_len) == 0) {
                s_on_cmd(evt->data, evt->data_len);
            }
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t wifi_mqtt_init(mqtt_cmd_cb_t on_command)
{
    s_on_cmd = on_command;

    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "device/%s/command", DEVICE_ID);
    snprintf(s_topic_status, sizeof(s_topic_status), "device/%s/status", DEVICE_ID);
    snprintf(s_topic_heartbeat, sizeof(s_topic_heartbeat), "device/%s/heartbeat", DEVICE_ID);

    s_wifi_event = xEventGroupCreate();
    if (!s_wifi_event) return ESP_ERR_NO_MEM;

    /* Initialize NVS (needed by WiFi) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs init");

    /* Network interface */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* WiFi */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));

    wifi_config_t sta_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init complete (device=%s)", DEVICE_ID);
    return ESP_OK;
}

bool wifi_mqtt_is_connected(void)
{
    return (s_wifi_event &&
            (xEventGroupGetBits(s_wifi_event) & (WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT)) ==
            (WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT));
}

const char *wifi_mqtt_get_ip(void)
{
    return s_ip_str;
}

int wifi_mqtt_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    }
    return s_rssi;
}

esp_err_t wifi_mqtt_publish(const char *topic, const char *payload, bool retained)
{
    if (!s_mqtt_client) return ESP_ERR_INVALID_STATE;
    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, retained ? 1 : 0);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}
