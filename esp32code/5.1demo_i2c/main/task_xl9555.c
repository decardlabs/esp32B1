#include "task_xl9555.h"


static const char *TAG = "task_xl9555";

void xl9555_task(void *pvParameters)
{
    uint32_t count = 0,key1_count = 0,key2_count = 0;
    uint8_t key1_level = 0,key2_level = 0;
    
    xl9555_init();

    xl9555_set_pin_dir(KEY_PORT, KEY1_PIN, true);  
    xl9555_set_pin_dir(KEY_PORT, KEY2_PIN, true);

    xl9555_set_pin_dir(LED_PORT, LED1_PIN, false);
    xl9555_set_pin_dir(LED_PORT, LED2_PIN, false);    
    xl9555_set_pin_dir(LED_PORT, LED3_PIN, false);
    xl9555_set_pin_dir(LED_PORT, LED4_PIN, false); 

    xl9555_set_pin_dir(BEEP_PORT, BEEP_PIN, false);    
    while(1)
    {
        count++;
        count = count % 200;
        if(0 == count)
        {
            xl9555_set_pin_level(LED_PORT, LED1_PIN, false);
            xl9555_set_pin_level(LED_PORT, LED2_PIN, false);
        }
        if(100 == count)
        {
            xl9555_set_pin_level(LED_PORT, LED1_PIN, true);
            xl9555_set_pin_level(LED_PORT, LED2_PIN, true);
        }
                                      
        xl9555_get_pin_level(KEY_PORT, KEY1_PIN, &key1_level);
        if(!key1_level)
        {           
            do{
                vTaskDelay(10 / portTICK_PERIOD_MS); 
                xl9555_get_pin_level(KEY_PORT, KEY1_PIN, &key1_level);
            }while(!key1_level);

            key1_count++;
            key1_count = key1_count % 2;
            if(key1_count== 0)
            {
                xl9555_set_pin_level(LED_PORT, LED3_PIN, true);
                xl9555_set_pin_level(LED_PORT, LED4_PIN, true);
            }
            else
            {
                xl9555_set_pin_level(LED_PORT, LED3_PIN, false);
                xl9555_set_pin_level(LED_PORT, LED4_PIN, false);
            }
        }

        xl9555_get_pin_level(KEY_PORT, KEY2_PIN, &key2_level);
        if(!key2_level)
        {           
            do{
                vTaskDelay(10 / portTICK_PERIOD_MS); 
                xl9555_get_pin_level(KEY_PORT, KEY2_PIN, &key2_level);
            }while(!key2_level);

            key2_count++;
            key2_count = key2_count % 2;
            if(key2_count== 0)
            {
                xl9555_set_pin_level(BEEP_PORT, BEEP_PIN, true);
            }
            else
            {
                xl9555_set_pin_level(BEEP_PORT, BEEP_PIN, false);
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

BaseType_t xl9555_task_create(void)
{
   return xTaskCreate(xl9555_task, "xl9555_task", 4096, NULL, 10, NULL);
}