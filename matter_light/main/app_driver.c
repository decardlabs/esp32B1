#include "app_driver.h"
#include "ws2812b.h"

static const char *TAG = "APP_DRIVER";

esp_err_t app_driver_set_power(bool on)
{
    if (on) {
        // ON = soft blue-white
        ESP_LOGI(TAG, "Light ON");
        return ws2812_set_all(80, 120, 180);
    } else {
        // OFF = all clear
        ESP_LOGI(TAG, "Light OFF");
        return ws2812_clear();
    }
}

esp_err_t app_driver_identify(void)
{
    for (int blink = 0; blink < 6; blink++) {
        app_driver_set_power(blink % 2);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    app_driver_set_power(false);
    return ESP_OK;
}
