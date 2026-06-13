#ifndef __TASK_SDCARD_H__
#define __TASK_SDCARD_H__

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
    SDCARD_DIALOG_ROLE_USER = 0,
    SDCARD_DIALOG_ROLE_ASSISTANT,
} sdcard_dialog_role_t;

BaseType_t sdcard_task_create(void);
esp_err_t sdcard_task_post_dialog(sdcard_dialog_role_t role, const char *text);
bool sdcard_task_is_ready(void);

#endif
