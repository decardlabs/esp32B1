#include "task_ledc.h"

#define BREATH_SG90_MODE   0  //0是呼吸灯模式   1是舵机模式

static const char *TAG = "TASK_LEDC";
QueueHandle_t ledc_queue_handle = NULL;


static void ledc_task(void *pvParameters)
{
    uint32_t duty = 0;

    ledc_queue_handle = xQueueCreate(10, sizeof(uint32_t));

    #if BREATH_SG90_MODE 
    bsp_ledc_sg90_init();
    #else   
    bsp_ledc_init(LEDC_FADE_MODE);
    #endif
   
    while (1)
    {
        #if BREATH_SG90_MODE        
        for (int a = 0; a <= 180; a++) {
            bsp_ledc_sg90_set_angle((uint8_t)a);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        for (int a = 180; a >= 0; a--) {
            bsp_ledc_sg90_set_angle((uint8_t)a);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        
        #else

        ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023, 2500);
        ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
        vTaskDelay(pdMS_TO_TICKS(300));
        ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, 2500);
        ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
        vTaskDelay(pdMS_TO_TICKS(300));
        // if(xQueueReceive(ledc_queue_handle, &duty, portMAX_DELAY) == pdPASS)
        // {
        //     duty = duty + 3;
        //     ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        //     ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        //     ESP_LOGI(TAG, "ledc duty: %d", duty);
        // }


        // vTaskDelay(100 / portTICK_PERIOD_MS);
        // ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        // ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        // duty += 4;
        // if(duty > 1023)
        // {
        //     duty = 0;
        // }
        // ESP_LOGI(TAG, "ledc duty: %d", duty);

        #endif
    }
}

BaseType_t ledc_task_create(void)
{
   return xTaskCreate(ledc_task, "ledc_task", 4096, NULL, 10, NULL);
}









