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
#include "bsp_gpio.h"
#include "task_led.h"

void app_main(void)
{
    bsp_led_init();
    bsp_key_init();
    led_static_task_handle = led_static_task_create();
    if(led_static_task_handle == NULL)
    {
        printf("led_task_create failed\n");
    }
    //xTaskCreatePinnedToCore(led_task, "led_task",2048*2, NULL, 10, &led_task_handle, 1);
    printf("\n***\nIO Test!\n***\n");
    while(1)
    {     
        if(bsp_gpio_get_level(KEY_GPIO) == 0)
        {
            vTaskDelay(20 / portTICK_PERIOD_MS);
            if(bsp_gpio_get_level(KEY_GPIO) == 0)
            {
                while(bsp_gpio_get_level(KEY_GPIO) == 0)
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
