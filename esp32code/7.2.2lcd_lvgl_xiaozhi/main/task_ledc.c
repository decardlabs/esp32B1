#include "task_ledc.h"

#define BREATH_SG90_MODE  1 // 1: 呼吸模式, 0: 舵机模式

void ledc_task(void *pvParameters)
{    
    uint32_t ledc_duty = 0;

    #if BREATH_SG90_MODE
    bsp_ledc_init(LEDC_FADE_MODE);
    #else
    bsp_ledc_sg90_init();
    #endif
    
    while (1)
    {
        
       #if BREATH_SG90_MODE
        // for(ledc_duty = 0; ledc_duty < 1024; ledc_duty+=4)
        // {
        //     ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, ledc_duty);
        //     ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);       
        //     vTaskDelay(10 / portTICK_PERIOD_MS);
        // }

        ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023, 2500);
        ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
        vTaskDelay(300 / portTICK_PERIOD_MS);

        ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0, 2500);
        ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
        vTaskDelay(300 / portTICK_PERIOD_MS);

        #else

        bsp_ledc_sg90_set_angle(90);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        bsp_ledc_sg90_set_angle(0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        bsp_ledc_sg90_set_angle(180);
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        #endif
    }
}



BaseType_t ledc_task_create(void)
{
    BaseType_t ret = xTaskCreate(ledc_task, "ledc_task",4096, NULL, 10, NULL);
    return ret;
}
