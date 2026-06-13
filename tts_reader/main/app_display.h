#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdbool.h>
#include "esp_err.h"

/* Display modes — matched to state machine */
typedef enum {
    DISP_MODE_FILE_SELECT,   /* 文件选择（上电默认）*/
    DISP_MODE_READY,         /* 文件已选，等待播放 */
    DISP_MODE_PLAYING,       /* 播放中 */
    DISP_MODE_PAUSED,        /* 已暂停 */
    DISP_MODE_NO_SD,         /* 未检测到TF卡 */
    DISP_MODE_NO_FILES,      /* TF卡无txt文件 */
} disp_mode_t;

esp_err_t app_display_init(void);

/* Update display content */
void app_display_set_mode(disp_mode_t mode);
void app_display_set_filename(const char *name);
void app_display_set_text(const char *text);

/* File selection */
void app_display_set_file_list(const char names[][64], int count, int selected);

/* Progress */
void app_display_set_progress(int current, int total);

/* Volume / Speed indicator (0~N, current selection) */
void app_display_set_volume(int vol_level, int vol_max);
void app_display_set_speed(int spd_level, int spd_max);

#endif
