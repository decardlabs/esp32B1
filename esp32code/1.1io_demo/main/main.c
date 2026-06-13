#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/gpio.h"

#define LED_PIN         GPIO_NUM_1   // LED连接的GPIO引脚
#define KEY_PIN         GPIO_NUM_40  // 按键连接的GPIO引脚


// 任务函数：控制LED闪烁
void led_task(void *pvParameters)
{
    while(1)
    {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(1000 / portTICK_PERIOD_MS); // 延时1秒
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS); // 延时1秒
    }
}

void app_main(void)
{
    printf("\n*****************\nIO TEST Demo!\n*****************\n");
        // 配置LED引脚
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_conf);
 
    // 配置按键引脚（启用内部上拉，未按下时为高电平）
    gpio_config_t key_conf = {
        .pin_bit_mask = (1ULL << KEY_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE`
    };
    gpio_config(&key_conf);

    xTaskCreate(
        led_task,   // 任务函数
        "led_task", // 任务名称
        2048,       // 任务栈大小（字节）
        NULL,       // 任务参数
        5,          // 任务优先级
        NULL        // 任务句柄
    );  
    
    while(1)
    {
        // 读取按键状态
        if(gpio_get_level(KEY_PIN) == 0) // 按键按下时为低电平
        {
            if (gpio_get_level(KEY_PIN) == 0) // 再次确认
            {
                // 等待释放
                while (gpio_get_level(KEY_PIN) == 0)
                {
                    vTaskDelay(10 / portTICK_PERIOD_MS); // 不要忙等
                }      
                vTaskDelay(20/ portTICK_PERIOD_MS); // 去抖释放
                printf("Key pressed\n");
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);    

    }

}
