#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "i2c_bus.h"
#include "xl9555.h"
#include "es8388.h"
#include "audio_player.h"
#include "wifi_mqtt.h"
#include "cmd_handler.h"

static const char *TAG = "MAIN";

/* MQTT command callback → cmd_handler */
static void on_mqtt_command(const char *payload, int len)
{
    cmd_handler_process(payload, len);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-S3 TTS Player ===");
    ESP_LOGI(TAG, "Device: %s", DEVICE_ID);

    /* ── Stage 1: I2C bus ─────────────────────────────────────────────── */
    ESP_LOGI(TAG, "[1/6] I2C bus init...");
    ESP_ERROR_CHECK(i2c_bus_init());

    /* ── Stage 2: XL9555 GPIO expander ────────────────────────────────── */
    ESP_LOGI(TAG, "[2/6] XL9555 init...");
    ESP_ERROR_CHECK(xl9555_init());

    /* ── Stage 3: ES8388 audio codec ──────────────────────────────────── */
    ESP_LOGI(TAG, "[3/6] ES8388 init...");
    ESP_ERROR_CHECK(es8388_init());

    /* ── Stage 4: Enable speaker amp ──────────────────────────────────── */
    ESP_LOGI(TAG, "[4/6] Speaker enable...");
    ESP_ERROR_CHECK(xl9555_speaker_enable(true));
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ── Stage 5: I2S audio player ────────────────────────────────────── */
    ESP_LOGI(TAG, "[5/6] Audio player init...");
    ESP_ERROR_CHECK(audio_player_init());
    audio_player_set_volume(0);   /* max volume at boot */

    /* ── Stage 6: WiFi ────────────────────────────────────────────── */
    ESP_LOGI(TAG, "[6/6] WiFi init...");
    ESP_ERROR_CHECK(wifi_mqtt_init(on_mqtt_command));

    /* ── Command handler (heartbeat task) ──────────────────────────────── */
    ESP_ERROR_CHECK(cmd_handler_init());

    ESP_LOGI(TAG, "=== System ready ===");
    ESP_LOGI(TAG, "Waiting for WiFi + MQTT...");

    /* Main loop — just monitor */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (wifi_mqtt_is_connected()) {
            static int counter = 0;
            if (++counter % 12 == 0) {  /* every ~60s */
                ESP_LOGI(TAG, "Heartbeat: ip=%s rssi=%d state=%s heap=%u",
                         wifi_mqtt_get_ip(),
                         wifi_mqtt_get_rssi(),
                         audio_player_get_state_str(),
                         (unsigned)esp_get_free_heap_size());
            }
        }
    }
}
