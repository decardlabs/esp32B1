#include "esp_log.h"
#include "task_key_led.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "test001 start");

    if (task_key_led_create() != pdPASS) {
        ESP_LOGE(TAG, "failed to create key-led task");
    }
}
