#include "task_ledc.h"

static const char *TAG = "TASK_LEDC";
QueueHandle_t ledc_queue_handle = NULL;


static void ledc_task(void *pvParameters)
{
    uint32_t duty = 0;

    ledc_queue_handle = xQueueCreate(10, sizeof(uint32_t));

    // bsp_ledc_duty_init();    
    // bsp_ledc_fade_init();
    bsp_ledc_sg90_init();
   
    while (1)
    {
        for (int a = 0; a <= 180; a += 5) {
            bsp_ledc_sg90_set_angle((uint8_t)a);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        for (int a = 180; a >= 0; a -= 5) {
            bsp_ledc_sg90_set_angle((uint8_t)a);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        
        
        // ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1024, 2000);
        // ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);

        // ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1, 2000);
        // ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);



        // if(xQueueReceive(ledc_queue_handle, &duty, portMAX_DELAY) == pdPASS)
        // {
        //     duty = duty + 4;
            // ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
            // ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
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
    }
}



BaseType_t ledc_task_create(void)
{
   return xTaskCreate(ledc_task, "ledc_task", 4096, NULL, 10, NULL);
}









