#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdbool.h>
#include "esp_err.h"

/* Display modes */
typedef enum {
    DISP_MODE_INIT = 0,
    DISP_MODE_USB_MSC,        /* U盘模式 */
    DISP_MODE_SCANNING,       /* 扫描中 */
    DISP_MODE_READING,        /* 阅读中 */
    DISP_MODE_PAUSED,         /* 已暂停 */
    DISP_MODE_FILE_DONE,      /* 播放完成 */
} disp_mode_t;

esp_err_t app_display_init(void);

/* Update display content */
void app_display_set_mode(disp_mode_t mode);
void app_display_set_filename(const char *name);
void app_display_set_text(const char *text);
void app_display_set_progress(int current, int total);

#endif
