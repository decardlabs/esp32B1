#include "cmd_handler.h"
#include "audio_player.h"
#include "http_stream.h"
#include "wifi_mqtt.h"
#include "board_pins.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "CMD";
static int s_volume_pct = 100;  /* 0-100, default max */

/* ── JSON helpers ─────────────────────────────────────────────────────── */

static const char *json_str(cJSON *obj, const char *key, const char *def)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v)) return v->valuestring;
    return def;
}

static int json_int(cJSON *obj, const char *key, int def)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(v)) return v->valueint;
    return def;
}

/* ── Status report ────────────────────────────────────────────────────── */

static void publish_status(void)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{"
        "\"device_id\":\"%s\","
        "\"online\":true,"
        "\"ip\":\"%s\","
        "\"wifi_rssi\":%d,"
        "\"volume\":%d,"
        "\"play_state\":\"%s\","
        "\"free_heap\":%u"
        "}",
        DEVICE_ID,
        wifi_mqtt_get_ip(),
        wifi_mqtt_get_rssi(),
        s_volume_pct,
        audio_player_get_state_str(),
        (unsigned)esp_get_free_heap_size()
    );
    char topic[64];
    snprintf(topic, sizeof(topic), "device/%s/status", DEVICE_ID);
    wifi_mqtt_publish(topic, buf, true);
    ESP_LOGI(TAG, "Status published");
}

static void publish_heartbeat(void)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{"
        "\"wifi_rssi\":%d,"
        "\"volume\":%d,"
        "\"play_state\":\"%s\","
        "\"free_heap\":%u"
        "}",
        wifi_mqtt_get_rssi(),
        s_volume_pct,
        audio_player_get_state_str(),
        (unsigned)esp_get_free_heap_size()
    );
    char topic[64];
    snprintf(topic, sizeof(topic), "device/%s/heartbeat", DEVICE_ID);
    wifi_mqtt_publish(topic, buf, false);
}

/* ── Command dispatch ─────────────────────────────────────────────────── */

void cmd_handler_process(const char *payload, int len)
{
    char *null_terminated = malloc(len + 1);
    if (!null_terminated) return;
    memcpy(null_terminated, payload, len);
    null_terminated[len] = '\0';

    ESP_LOGI(TAG, "CMD: %s", null_terminated);

    cJSON *root = cJSON_Parse(null_terminated);
    free(null_terminated);

    if (!root) {
        ESP_LOGE(TAG, "JSON parse error");
        return;
    }

    const char *cmd = json_str(root, "cmd", "");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (strcmp(cmd, "play") == 0) {
        const char *url = "";
        if (params) url = json_str(params, "url", "");
        if (strlen(url) > 0) {
            audio_player_stop();
            vTaskDelay(pdMS_TO_TICKS(30));
            http_stream_start(url, NULL, NULL);
        }
    } else if (strcmp(cmd, "tts") == 0) {
        const char *url = json_str(root, "url", "");
        if (strlen(url) > 0) {
            audio_player_stop();
            vTaskDelay(pdMS_TO_TICKS(30));
            http_stream_start(url, NULL, NULL);
        } else {
            ESP_LOGW(TAG, "TTS url missing");
        }
    } else if (strcmp(cmd, "pause") == 0) {
        audio_player_pause();
    } else if (strcmp(cmd, "resume") == 0) {
        audio_player_resume();
    } else if (strcmp(cmd, "stop") == 0) {
        http_stream_abort();
        audio_player_stop();
    } else if (strcmp(cmd, "volume") == 0) {
        if (params) s_volume_pct = json_int(params, "value", s_volume_pct);
        if (s_volume_pct < 0) s_volume_pct = 0;
        if (s_volume_pct > 100) s_volume_pct = 100;
        /* Map 0-100% → -96..0 dB linearly: 100% = 0dB (max power) */
        int db = s_volume_pct - 100;
        if (db < -96) db = -96;
        audio_player_set_volume(db);
        ESP_LOGI(TAG, "Volume: %d%% (%d dB)", s_volume_pct, db);
    } else if (strcmp(cmd, "tone") == 0) {
        uint16_t freq = 1000;
        uint16_t duration = 1200;
        if (params) {
            freq = (uint16_t)json_int(params, "freq", 1000);
            duration = (uint16_t)json_int(params, "duration", 1200);
        }
        audio_player_play_tone(freq, duration);
    } else if (strcmp(cmd, "status") == 0) {
        publish_status();
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd);
    }

    publish_status();
    cJSON_Delete(root);
}

/* ── Heartbeat timer ──────────────────────────────────────────────────── */

static void heartbeat_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (wifi_mqtt_is_connected()) {
            publish_heartbeat();
        }
    }
}

esp_err_t cmd_handler_init(void)
{
    xTaskCreate(heartbeat_task, "heartbeat", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "Command handler ready");
    return ESP_OK;
}
