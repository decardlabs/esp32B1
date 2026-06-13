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

void app_main(void)
{
    bool led_status = 0;

    bsp_led_init();
    bsp_key_init();
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
               
                led_status = !led_status;
            }

        }
        gpio_set_level(LED_GPIO,led_status);
        led_status = !led_status;
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
