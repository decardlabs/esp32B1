#include "sd_card.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static const char *TAG = "SD_CARD";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

esp_err_t sd_card_init(sdmmc_card_t **out_card)
{
    if (s_mounted) {
        *out_card = s_card;
        return ESP_OK;
    }

    esp_err_t ret;

    /* SDSPI host config (SPI2_HOST, already inited by main.c with larger max_transfer_sz). */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    /* SDSPI device config */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cs = PIN_TF_CS;

    /* Mount config */
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
    };

    /* Mount FAT filesystem with VFS register in one call */
    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        s_card = NULL;
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card: %llu MB",
             (uint64_t)s_card->csd.capacity * 512 / (1024 * 1024));

    s_mounted = true;
    *out_card = s_card;
    ESP_LOGI(TAG, "SD card mounted at /sdcard");
    return ESP_OK;
}

void sd_card_deinit(void)
{
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
        s_card = NULL;
        s_mounted = false;
    }
}

sdmmc_card_t *sd_card_get_handle(void)
{
    return s_card;
}
