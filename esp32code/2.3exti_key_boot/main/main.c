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
#include "bsp_exti.h"


void app_main(void)
{
    printf("\n***\nEXTI Test!\n***\n");
    bsp_led_init();
    // bsp_key_init();
    bsp_exti_init(KEY_GPIO);
    bsp_exti_init(BOOT_GPIO);
    while(1)
    {     

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
