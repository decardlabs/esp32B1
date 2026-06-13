#ifndef __TF_SDCARD_H__
#define __TF_SDCARD_H__

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#define TF_SDCARD_MOUNT_POINT      "/sdcard"
/*
 * sdkconfig currently uses CONFIG_FATFS_LFN_NONE=y, so the log filename must
 * stay in 8.3 format.
 */
#define TF_SDCARD_LOG_FILE_PATH    TF_SDCARD_MOUNT_POINT "/dialog.txt"

/*
 * Schematic note:
 * TF_CS and KEY0 are tied to ESP32-S3 IO40 on the board.
 */
#define TF_SDCARD_CS_GPIO          GPIO_NUM_40

esp_err_t tf_sdcard_mount(void);
esp_err_t tf_sdcard_unmount(void);
bool tf_sdcard_is_mounted(void);
const char *tf_sdcard_get_mount_point(void);
const char *tf_sdcard_get_log_file_path(void);
esp_err_t tf_sdcard_append_text(const char *text);

#endif
