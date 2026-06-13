#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"

#include "lcd_st7796.h"
#include "sd_card.h"
#include "tusb_msc.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "USB MSC Demo starting...");

    /* ── Step 1: Init I2C + XL9555 + SPI2 LCD, show static text ── */
    ESP_ERROR_CHECK(lcd_init_and_show_text());
    ESP_LOGI(TAG, "LCD shows static text — display will not change");

    /* ── Step 2: Init SD card on same SPI2 bus (non-fatal if absent) ── */
    sdmmc_card_t *card = NULL;
    esp_err_t sd_ret = sd_card_init(lcd_get_spi_host(), &card);
    if (sd_ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card init failed (%s) — continuing without", esp_err_to_name(sd_ret));
    } else {
        ESP_LOGI(TAG, "TF card ready");

        /* ── Step 3: Init TinyUSB MSC and expose card to USB host ── */
        esp_err_t usb_ret = tusb_msc_init(card);
        if (usb_ret != ESP_OK) {
            ESP_LOGW(TAG, "USB MSC init failed (%s)", esp_err_to_name(usb_ret));
        } else {
            ESP_LOGI(TAG, "USB MSC active — plug into PC to see the drive");
        }
    }

    /* Main loop: nothing to do — TinyUSB runs in background */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
