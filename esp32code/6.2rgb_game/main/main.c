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
#include "bsp_uart.h"
#include "bsp_gptimer.h"
#include "task_timer.h"
#include "task_ledc.h"
#include "task_oled.h"
#include "task_xl9555.h"
#include "task_ws2812.h"
#include "task_dht11.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "WS2812 RGB match game");
    bsp_led_init();
    //bsp_key_init();
    bsp_i2c0_init();    
    //exti_task_prco_create();
    // uart1_task_create();

    // led_task_create();
    // timer_task_create();

    // ledc_task_create();
    // oled_task_create();
    // xl9555_task_create();
    ws2812_task_create();
    // dht11_task_create();
        
    while(1)
    {     
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
