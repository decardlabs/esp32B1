#include "sd_card.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include <string.h>

static const char *TAG = "SD_CARD";

esp_err_t sd_card_init(int spi_host, sdmmc_card_t **out_card)
{
    sdmmc_card_t *card = NULL;
    esp_err_t ret;

    /* Init sdspi driver (won't init bus — already done by LCD) */
    ESP_RETURN_ON_ERROR(sdspi_host_init(), TAG, "sdspi host init");

    /* Add SD card device to the existing SPI bus */
    sdspi_device_config_t dev_cfg = {
        .host_id = spi_host,
        .gpio_cs = PIN_TF_CS,
        .gpio_cd = GPIO_NUM_NC,
        .gpio_wp = GPIO_NUM_NC,
        .gpio_int = GPIO_NUM_NC,
    };
    sdspi_dev_handle_t sd_handle;
    bool device_added = false;
    ret = sdspi_host_init_device(&dev_cfg, &sd_handle);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "sdspi add device");
    device_added = true;

    /* Init the card */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = spi_host;

    card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    ESP_GOTO_ON_FALSE(card, ESP_ERR_NO_MEM, err, TAG, "card malloc");

    ret = sdmmc_card_init(&host, card);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "sdmmc card init");

    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD card ready: %llu sectors (%llu MB)",
             (uint64_t)card->csd.capacity,
             (uint64_t)card->csd.capacity * 512 / (1024 * 1024));

    *out_card = card;
    return ESP_OK;

err:
    if (card) free(card);
    if (device_added) sdspi_host_remove_device(sd_handle);
    return ret;
}
