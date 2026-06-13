/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "bsp_gpio.h"

TaskHandle_t led_task_handle = NULL;
// struct tskTaskControlBlock * led_task_handle = NULL;    //int *a = 0;
// struct tskTaskControlBlock  led_task_handle = NULL;   //  int a =0;

StackType_t led_static_task_buffer[2048*2];
StaticTask_t led_static_task_tcb;
TaskHandle_t led_static_task_handle = NULL;

void led_task(void *pvParameters)
{
    bool led_status = 0;
    TaskHandle_t handle;
    // uint8_t count = 0;
    while (1)
    {
        gpio_set_level(LED_GPIO,led_status);
        led_status = !led_status;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        printf("task led_status: %d\n", led_status);  

        // 打印当前任务剩余栈空间（单位：Byte）
        UBaseType_t stack_remain = uxTaskGetStackHighWaterMark(NULL);
        printf("Remaining stack of led_task: %u Byte\n",stack_remain); 

        handle = xTaskGetCurrentTaskHandle();  // 获取当前任务句柄 
        printf("Current task handle = %p\n", handle);
        
        UBaseType_t priority = uxTaskPriorityGet(handle);
        printf("Current task priority = %u\n", priority);

        printf("Running on core: %d\n", xPortGetCoreID());   // 打印当前核心 ID
        // if(count++ >= 5)
        // {
        //     break;
        // }
    }
    printf("led_task end\n");
    vTaskDelete(NULL);
}

void app_main(void)
{
    bsp_led_init();
    bsp_key_init();

    //xTaskCreate(led_task, "led_task",2048*2, NULL, 10, &led_task_handle);
    led_static_task_handle = xTaskCreateStatic(led_task, "led_task",2048*2, NULL, 10, led_static_task_buffer, &led_static_task_tcb);
    if(led_static_task_handle == NULL)
    {
        printf("led_static_task_handle is NULL\n");
    }
    //xTaskCreatePinnedToCore(led_task, "led_task",2048*2, NULL, 10, &led_task_handle, 1);
    printf("\n***\nIO Test!\n***\n");
    while(1)
    {     
        if(gpio_get_level(KEY_GPIO) == 0)
        {
            vTaskDelay(20 / portTICK_PERIOD_MS);
            if(gpio_get_level(KEY_GPIO) == 0)
            {
                while(gpio_get_level(KEY_GPIO) == 0)
                {
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                }
                vTaskDelay(20 / portTICK_PERIOD_MS);
                printf("Key pressed\n");
                vTaskPrioritySet(led_static_task_handle, 15);
                // vTaskDelete(led_task_handle);

            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
