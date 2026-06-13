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
#include "task_exti_prco.h"
#include "task_uart.h"
#include "esp_log.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "\n***\nUART1 Test!\n***\n");
    BaseType_t ret = 0;
    bsp_led_init();
    // bsp_key_init();
    //ret = exti_task_prco_create();
    // if(ret != pdPASS)
    // {
    //     printf("exti_task_prco_create failed\n");
    // }


    ret = uart1_task_create();
    if(ret != pdPASS)
    {
        ESP_LOGE(TAG, "uart1_task_create failed\n");
    }
    while(1)
    {     
        vTaskDelay(10 / portTICK_PERIOD_MS);

    }
}
