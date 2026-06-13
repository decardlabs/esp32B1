#include "task_ws2812.h"


static const char *TAG = "TASK_WS2812";

void ws2812_task(void *pvParameters)
{
    bool led_on_off = false;
    ws2812_txs0108_enable();
    led_strip_handle_t ws2812_handle = ws2812_init();
    ESP_LOGI(TAG, "Start blinking LED strip");
    while(1)
    {
        if (led_on_off) {
            /* Set the LED pixel using RGB from 0 (0%) to 255 (100%) for each color */
            for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
                ESP_ERROR_CHECK(led_strip_set_pixel(ws2812_handle, i, 5, 5, 5));
            }
            /* Refresh the strip to send data */
            ESP_ERROR_CHECK(led_strip_refresh(ws2812_handle));
            ESP_LOGI(TAG, "LED ON!");
        } else {
            /* Set all LED off to clear all pixels */
            ESP_ERROR_CHECK(led_strip_clear(ws2812_handle));
            ESP_LOGI(TAG, "LED OFF!");
        }

        led_on_off = !led_on_off;
    
        effect_breathing(0, 50, 50);
        effect_comet();
        // effect_rainbow();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
    


BaseType_t ws2812_task_create(void)
{
    return xTaskCreate(ws2812_task, "ws2812_task",4096, NULL, 10, NULL);
}

