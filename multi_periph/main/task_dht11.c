#include "task_dht11.h"


static const char *TAG = "TASK_DHT11";
oled_msg_t dht11_msg = {0};
void dht11_task(void *pvParameters)
{
    float temperature, humidity;

    while (1)
    {
        if (dht_read_float_data(DHT_TYPE_DHT11, DHT_GPIO_PIN, &humidity, &temperature) == ESP_OK)
            ESP_LOGI(TAG, "Humidity: %.1f%% Temp: %.1fC", humidity, temperature);
        else
            ESP_LOGI(TAG, "Could not read data from sensor");
        vTaskDelay(pdMS_TO_TICKS(2000));
        dht11_msg.type = OLED_MSG_TYPE_DHT11;
        dht11_msg.u_data.dht11.temp = (uint16_t)temperature;
        dht11_msg.u_data.dht11.humi = (uint16_t)humidity;
        oled_send_msg(&dht11_msg, portMAX_DELAY);
    }
}


BaseType_t dht11_task_create(void)
{
    return xTaskCreate(dht11_task, "dht11_task",4096, NULL, 10, NULL);
}