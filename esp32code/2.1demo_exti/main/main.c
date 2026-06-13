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
#include "task_exti.h"

/**
 * @brief 主应用程序入口函数
 * 
 * 程序启动后首先初始化硬件资源，然后创建任务并进入主循环。
 * 主要功能是测试外部中断（EXIT）功能，通过按键触发中断来控制LED。
 */
void app_main(void)
{
    // 打印程序启动信息
    printf("\n***\nEXIT Test!\n***\n");
    
    // 初始化BOOT按键IO口
    bsp_bootio_init();
    
    // 初始化LED IO口
    bsp_led_init();
    
    // 注释掉的按键初始化函数（可能由外部中断替代）
    //bsp_key_init();
    
    // 初始化外部中断，配置KEY_GPIO引脚为中断输入模式
    bsp_exti_init(KEY_GPIO);
    
    // 初始化外部中断，配置BOOT_GPIO引脚为中断输入模式
    bsp_exti_init(BOOT_GPIO);    

    // 创建外部中断处理任务
    BaseType_t ret = exti_proc_task_create();
    if(ret != pdPASS)
    {
        // 如果任务创建失败，则打印错误信息
        printf("exti_proc_task_create failed\n");
    }

    // 主循环，保持程序运行
    while(1)
    {     
        // 延时10毫秒，避免过度占用CPU
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}