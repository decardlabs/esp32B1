#include "task_xl9555.h"




void xl9555_task(void *pvParameters)
{
    uint32_t led_count = 0;
    bool key1_level = 0;
    bool key2_level = 0;
    uint8_t key1_cnt = 0,key2_cnt = 0;
    xl9555_init();
    xl9555_set_pin_dir(LED_PORT,LED1_PIN,0);
    xl9555_set_pin_dir(LED_PORT,LED2_PIN,0);
    xl9555_set_pin_dir(LED_PORT,LED3_PIN,0);
    xl9555_set_pin_dir(LED_PORT,LED4_PIN,0);
    xl9555_set_pin_dir(BEEP_PORT,BEEP_PIN,0);


    xl9555_set_pin_dir(KEY_PORT,KEY1_PIN,1);
    xl9555_set_pin_dir(KEY_PORT,KEY2_PIN,1);

    xl9555_set_pin_level(LED_PORT,LED1_PIN,1);
    xl9555_set_pin_level(LED_PORT,LED2_PIN,1);
    xl9555_set_pin_level(LED_PORT,LED3_PIN,1);
    xl9555_set_pin_level(LED_PORT,LED4_PIN,1);
    xl9555_set_pin_level(BEEP_PORT,BEEP_PIN,1);

    while(1)
    {
        led_count++;
        led_count %= 100;
        if(led_count == 0)
        {
            xl9555_set_pin_level(LED_PORT,LED1_PIN,0);
            xl9555_set_pin_level(LED_PORT,LED2_PIN,0);
        }
        if(led_count == 50)
        {
            xl9555_set_pin_level(LED_PORT,LED1_PIN,1);
            xl9555_set_pin_level(LED_PORT,LED2_PIN,1);
        }

        xl9555_get_pin_level(KEY_PORT,KEY1_PIN,&key1_level);
        xl9555_get_pin_level(KEY_PORT,KEY2_PIN,&key2_level);
        if(key1_level == 0)
        {
            do{
                vTaskDelay(pdMS_TO_TICKS(10));
                xl9555_get_pin_level(KEY_PORT,KEY1_PIN,&key1_level);
            }while(key1_level == 0);
            key1_cnt++;
            key1_cnt %=2;
            if(key1_cnt == 0)
            {
                xl9555_set_pin_level(LED_PORT,LED3_PIN,1);
                xl9555_set_pin_level(LED_PORT,LED4_PIN,1);
            }
            else
            {
                xl9555_set_pin_level(LED_PORT,LED3_PIN,0);
                xl9555_set_pin_level(LED_PORT,LED4_PIN,0);
            }
        }

        if(key2_level == 0)
        {
            do
            {
                vTaskDelay(pdMS_TO_TICKS(10));
                xl9555_get_pin_level(KEY_PORT,KEY2_PIN,&key2_level);
            } while (key2_level == 0);
            key2_cnt++;
            key2_cnt %=2;
            if(key2_cnt == 0)
            {
                xl9555_set_pin_level(BEEP_PORT,BEEP_PIN,1);
            }
            else
            {
                xl9555_set_pin_level(BEEP_PORT,BEEP_PIN,0);
            }
            
        }


        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


BaseType_t xl9555_task_create(void)
{
    return  xTaskCreate(xl9555_task, "xl9555_task",4096, NULL, 10, NULL);
}

