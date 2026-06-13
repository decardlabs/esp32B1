#include "tf_sdcard.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "bsp_spi.h"
#include "driver/sdspi_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "TF_SDCARD";

static sdmmc_card_t *s_tf_card = NULL;
static bool s_tf_card_mounted = false;

esp_err_t tf_sdcard_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_vfs_fat_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    esp_err_t ret;

    if (s_tf_card_mounted) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(bsp_spi2_lcd_is_initialized(), ESP_ERR_INVALID_STATE, TAG, "call bsp_spi2_lcd_init() first");

    host.slot = bsp_get_spi2_lcd_host();

    slot_config.host_id = bsp_get_spi2_lcd_host();
    slot_config.gpio_cs = TF_SDCARD_CS_GPIO;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;
    slot_config.gpio_int = GPIO_NUM_NC;

    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    ESP_RETURN_ON_ERROR(bsp_spi2_bus_lock(portMAX_DELAY), TAG, "spi2 bus lock failed");
    ret = esp_vfs_fat_sdspi_mount(TF_SDCARD_MOUNT_POINT,
                                  &host,
                                  &slot_config,
                                  &mount_config,
                                  &s_tf_card);
    bsp_spi2_bus_unlock();
    ESP_RETURN_ON_ERROR(ret, TAG, "mount tf card failed");

    s_tf_card_mounted = true;
    ESP_LOGI(TAG, "TF card mounted at %s", TF_SDCARD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t tf_sdcard_unmount(void)
{
    if (!s_tf_card_mounted || (s_tf_card == NULL)) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(bsp_spi2_bus_lock(portMAX_DELAY));
    ESP_ERROR_CHECK(esp_vfs_fat_sdcard_unmount(TF_SDCARD_MOUNT_POINT, s_tf_card));
    bsp_spi2_bus_unlock();
    s_tf_card = NULL;
    s_tf_card_mounted = false;
    ESP_LOGI(TAG, "TF card unmounted");
    return ESP_OK;
}

bool tf_sdcard_is_mounted(void)
{
    return s_tf_card_mounted;
}

const char *tf_sdcard_get_mount_point(void)
{
    return TF_SDCARD_MOUNT_POINT;
}

const char *tf_sdcard_get_log_file_path(void)
{
    return TF_SDCARD_LOG_FILE_PATH;
}

esp_err_t tf_sdcard_append_text(const char *text)
{
    FILE *file = NULL;

    ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, TAG, "text is null");
    ESP_RETURN_ON_FALSE(text[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "text is empty");
    ESP_RETURN_ON_FALSE(s_tf_card_mounted, ESP_ERR_INVALID_STATE, TAG, "tf card not mounted");
    ESP_RETURN_ON_ERROR(bsp_spi2_bus_lock(portMAX_DELAY), TAG, "spi2 bus lock failed");

    file = fopen(TF_SDCARD_LOG_FILE_PATH, "a");
    if (file == NULL) {
        bsp_spi2_bus_unlock();
        ESP_LOGE(TAG,
                 "open log file failed: %s, errno=%d (%s)",
                 TF_SDCARD_LOG_FILE_PATH,
                 errno,
                 strerror(errno));
        return ESP_FAIL;
    }

    if (fputs(text, file) < 0) {
        fclose(file);
        bsp_spi2_bus_unlock();
        ESP_LOGE(TAG, "append log text failed");
        return ESP_FAIL;
    }

    fclose(file);
    bsp_spi2_bus_unlock();
    return ESP_OK;
}
