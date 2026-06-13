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
#include "esp_log.h"
#include "esp_system.h"
#include "bsp_board_pins.h"
#include "bsp_i2c.h"
#include "bsp_gpio.h"
#include "bsp_spi.h"
#include "task_led.h"
#include "bsp_exti.h"
#include "task_exti_prco.h"
#include "task_uart.h"
#include "bsp_uart.h"
#include "bsp_gptimer.h"
#include "task_timer.h"
#include "task_ledc.h"
#include "task_oled.h"
#include "task_ws2812.h"
#include "task_dht11.h"
#include "lcd_st7796.h"
#include "task_lcd_lvgl.h"
#include "xl9555.h"

static const char *TAG = "MAIN";

#define APP_PROFILE_LCD_UI        1
#define APP_PROFILE_SENSOR_OLED   2
#define APP_PROFILE_WS2812        3
#define APP_PROFILE_EXTI_LED      4

#if CONFIG_APP_PROFILE_LCD_UI
#define APP_PROFILE APP_PROFILE_LCD_UI
#elif CONFIG_APP_PROFILE_SENSOR_OLED
#define APP_PROFILE APP_PROFILE_SENSOR_OLED
#elif CONFIG_APP_PROFILE_WS2812
#define APP_PROFILE APP_PROFILE_WS2812
#elif CONFIG_APP_PROFILE_EXTI_LED
#define APP_PROFILE APP_PROFILE_EXTI_LED
#else
#error "No app profile selected in menuconfig"
#endif

/*
 * Compile-time pin conflict guards.
 * Keep these feature switches in sync with the selected app profile.
 */
#define APP_FEATURE_LCD_UI      (APP_PROFILE == APP_PROFILE_LCD_UI)
#define APP_FEATURE_DHT11       (APP_PROFILE == APP_PROFILE_SENSOR_OLED)
#define APP_FEATURE_WS2812      (APP_PROFILE == APP_PROFILE_WS2812)
#define APP_FEATURE_KEY0_INPUT  (APP_PROFILE == APP_PROFILE_EXTI_LED)

/* Optional features are controlled by menuconfig switches. */
#define APP_FEATURE_UART1       CONFIG_APP_FEATURE_UART1
#define APP_FEATURE_CAMERA      CONFIG_APP_FEATURE_CAMERA
#define APP_FEATURE_TF_SD       CONFIG_APP_FEATURE_TF_SD

#if APP_FEATURE_DHT11 && APP_FEATURE_LCD_UI
#if BSP_GPIO_DHT11_DATA == BSP_GPIO_LCD_DC
#error "Pin conflict: DHT11 DATA and LCD DC share GPIO1."
#endif
#endif

#if APP_FEATURE_KEY0_INPUT && APP_FEATURE_TF_SD
#if BSP_GPIO_KEY0 == BSP_GPIO_TF_SD_CS
#error "Pin conflict: KEY0 and TF CS share GPIO40."
#endif
#endif

#if APP_FEATURE_UART1 && APP_FEATURE_CAMERA
#if (BSP_GPIO_UART1_TX == BSP_GPIO_CAMERA_D6) || (BSP_GPIO_UART1_RX == BSP_GPIO_CAMERA_D7)
#error "Pin conflict: UART1 TX/RX share GPIO17/GPIO18 with camera D6/D7."
#endif
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "app start, profile=%d", APP_PROFILE);

#if APP_PROFILE == APP_PROFILE_LCD_UI
    // LCD + LVGL (+ optional UART/Timer) profile.
    bsp_i2c0_init();
    ESP_ERROR_CHECK(xl9555_init());
    ESP_ERROR_CHECK(xl9555_ws2812_level_shifter_enable());
    ESP_ERROR_CHECK(bsp_spi2_lcd_init());
    ESP_ERROR_CHECK(lcd_st7796_init());
    lcd_lvgl_task_create();
    // uart1_task_create();
    // timer_task_create();

#elif APP_PROFILE == APP_PROFILE_SENSOR_OLED
    // OLED + DHT11 profile. Do not enable LCD path in this profile.
    bsp_i2c0_init();
    ESP_ERROR_CHECK(xl9555_init());
    oled_task_create();
    dht11_task_create();

#elif APP_PROFILE == APP_PROFILE_WS2812
    // WS2812 effect profile.
    bsp_i2c0_init();
    ESP_ERROR_CHECK(xl9555_init());
    ws2812_task_create();
    // timer_task_create();

#elif APP_PROFILE == APP_PROFILE_EXTI_LED
    // EXTI + LED toggle profile.
    exti_task_prco_create();
    led_task_create();

#else
#error "Unsupported APP_PROFILE value"
#endif
        
    while(1)
    {     
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
