#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "bsp_board_pins.h"
#include "bsp_i2c.h"
#include "bsp_spi.h"
#include "lcd_st7796.h"
#include "task_lcd_lvgl.h"
#include "ws2812b.h"
#include "xl9555.h"
#include "app_matter.h"
#include "app_driver.h"

static const char *TAG = "MAIN";

/* LVGL status update (for factory reset notification) */
extern void lcd_lvgl_update_status(const char *status);

#define FACTORY_RESET_GPIO      BSP_GPIO_KEY0      /* GPIO 40 */
#define FACTORY_RESET_HOLD_MS   3000                /* 3 second hold */
#define FACTORY_RESET_CHECK_MS  50                  /* poll interval */

static void factory_reset_task(void *arg)
{
    int pressed_ms = 0;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << FACTORY_RESET_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    while (1) {
        if (gpio_get_level(FACTORY_RESET_GPIO) == 0) {
            pressed_ms += FACTORY_RESET_CHECK_MS;
            if (pressed_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "KEY0 held %dms - factory reset!", pressed_ms);
                lcd_lvgl_update_status("Factory reset...");
                vTaskDelay(pdMS_TO_TICKS(500));
                app_matter_factory_reset();
                /* Should not reach here - factory reset triggers restart */
                vTaskDelay(pdMS_TO_TICKS(5000));
                esp_restart();
            }
        } else {
            pressed_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_CHECK_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-Matter On/Off Light starting");

    /* 1. Initialize NVS (required by Matter) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Initialize network interfaces (required by Matter) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. Initialize I2C0 (for XL9555 IO expander) */
    bsp_i2c0_init();

    /* 4. Initialize XL9555 IO expander */
    ESP_ERROR_CHECK(xl9555_init());
    ESP_ERROR_CHECK(xl9555_ws2812_level_shifter_enable());

    /* 5. Initialize SPI2 LCD bus */
    ESP_ERROR_CHECK(bsp_spi2_lcd_init());

    /* 6. Initialize ST7796 LCD */
    ESP_ERROR_CHECK(lcd_st7796_init());

    /* 7. Start LVGL display task (shows QR + status) */
    lcd_lvgl_task_create();

    /* 7b. Start factory reset monitor (KEY0 long-press on GPIO 40) */
    xTaskCreate(factory_reset_task, "factory_reset", 4096, NULL, 1, NULL);

    /* 8. Initialize WS2812B (Matter light output) */
    ws2812_init();
    ws2812_clear();

    /* 9. Wait for LVGL to be ready */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 10. Start Matter stack (creates endpoint, starts BLE advertising) */
    ret = app_matter_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Matter start failed, rebooting in 10s...");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    ESP_LOGI(TAG, "System ready - scan QR code to commission");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
