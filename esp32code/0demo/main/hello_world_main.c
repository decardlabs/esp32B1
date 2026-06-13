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
#include "esp_heap_caps.h"
#include "esp_log.h"

void app_main(void)
{
    printf("Function: %s, File: %s, Line: %d\n", __func__, __FILE__, __LINE__);
    printf("\n*****************\nHello world!\n*****************\n");
printf("Function: %s, File: %s, Line: %d\n", __func__, __FILE__, __LINE__);
    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        return;
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());



    printf("===== Internal RAM =====\n");
    printf("Free heap: %lu bytes\n", esp_get_free_heap_size());
    printf("Total internal RAM: %zu bytes\n", heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    printf("Largest internal free block: %zu bytes\n",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    printf("\n===== PSRAM (SPIRAM) =====\n");

    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    if (psram_total == 0) {
        printf("No PSRAM detected.\n");
    } else {
        printf("Total PSRAM: %zu bytes\n", psram_total);
        printf("Free PSRAM: %zu bytes\n", psram_free);
        printf("Largest free PSRAM block: %zu bytes\n",heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }


    for (int i = 10; i >= 0; i--) {
        printf("Restarting in %d seconds...\n", i);
        printf("Function: %s, File: %s, Line: %d\n", __func__, __FILE__, __LINE__);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
}
