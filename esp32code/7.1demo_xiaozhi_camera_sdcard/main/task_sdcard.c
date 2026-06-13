#include "task_sdcard.h"
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/queue.h"
#include "tf_sdcard.h"

#define SDCARD_TASK_STACK            6144
#define SDCARD_TASK_PRIO             5
#define SDCARD_TASK_QUEUE_LEN        16
#define SDCARD_DIALOG_TIME_MAX_LEN   32
#define SDCARD_DIALOG_TEXT_MAX_LEN   256
#define SDCARD_DIALOG_VALID_EPOCH    1704067200
#define SDCARD_DIALOG_TIMEZONE       "CST-8"

typedef struct {
    uint32_t timestamp_ms;
    sdcard_dialog_role_t role;
    char timestamp_text[SDCARD_DIALOG_TIME_MAX_LEN];
    char text[SDCARD_DIALOG_TEXT_MAX_LEN];
} sdcard_dialog_msg_t;

static const char *TAG = "TASK_SDCARD";

static QueueHandle_t s_sdcard_queue = NULL;
static TaskHandle_t s_sdcard_task_handle = NULL;
static bool s_sdcard_task_ready = false;
static bool s_sdcard_timezone_ready = false;

static const char *sdcard_dialog_role_name(sdcard_dialog_role_t role)
{
    switch (role) {
        case SDCARD_DIALOG_ROLE_USER:
            return "USER";
        case SDCARD_DIALOG_ROLE_ASSISTANT:
            return "ASSISTANT";
        default:
            return "UNKNOWN";
    }
}

static esp_err_t sdcard_task_try_mount(void)
{
    esp_err_t ret = tf_sdcard_mount();

    if (ret == ESP_OK) {
        s_sdcard_task_ready = true;
    } else {
        s_sdcard_task_ready = false;
    }

    return ret;
}

static void sdcard_task_setup_timezone(void)
{
    if (s_sdcard_timezone_ready) {
        return;
    }

    if (setenv("TZ", SDCARD_DIALOG_TIMEZONE, 1) != 0) {
        ESP_LOGW(TAG, "set timezone failed, dialog log will use fallback timestamp");
        return;
    }

    tzset();
    s_sdcard_timezone_ready = true;
}

static void sdcard_task_fill_timestamp(sdcard_dialog_msg_t *msg)
{
    time_t now = 0;
    struct tm local_time = {0};

    if (msg == NULL) {
        return;
    }

    msg->timestamp_ms = esp_log_timestamp();
    msg->timestamp_text[0] = '\0';

    sdcard_task_setup_timezone();
    now = time(NULL);
    if ((now >= (time_t)SDCARD_DIALOG_VALID_EPOCH)
        && (localtime_r(&now, &local_time) != NULL)
        && (strftime(msg->timestamp_text,
                     sizeof(msg->timestamp_text),
                     "%Y-%m-%d %H:%M:%S UTC+8",
                     &local_time) > 0)) {
        return;
    }

    snprintf(msg->timestamp_text,
             sizeof(msg->timestamp_text),
             "unsynced +%" PRIu32 " ms",
             msg->timestamp_ms);
}

static esp_err_t sdcard_task_append_dialog(const sdcard_dialog_msg_t *msg)
{
    char line[SDCARD_DIALOG_TEXT_MAX_LEN + SDCARD_DIALOG_TIME_MAX_LEN + 32];

    if (msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(line,
             sizeof(line),
             "[%s] %s: %s\n",
             msg->timestamp_text,
             sdcard_dialog_role_name(msg->role),
             msg->text);

    return tf_sdcard_append_text(line);
}

static void sdcard_task(void *pvParameters)
{
    sdcard_dialog_msg_t msg;

    (void)pvParameters;

    if (sdcard_task_try_mount() != ESP_OK) {
        ESP_LOGW(TAG, "TF card mount failed at boot, will retry when messages arrive");
    } else {
        ESP_LOGI(TAG, "dialog log path: %s", tf_sdcard_get_log_file_path());
    }

    while (1) {
        if (xQueueReceive(s_sdcard_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!tf_sdcard_is_mounted()) {
            if (sdcard_task_try_mount() != ESP_OK) {
                ESP_LOGW(TAG, "drop dialog because TF card is unavailable");
                continue;
            }
        }

        if (sdcard_task_append_dialog(&msg) != ESP_OK) {
            s_sdcard_task_ready = false;
            ESP_LOGE(TAG, "append dialog failed");
        }
    }
}

BaseType_t sdcard_task_create(void)
{
    if (s_sdcard_task_handle != NULL) {
        return pdPASS;
    }

    if (s_sdcard_queue == NULL) {
        s_sdcard_queue = xQueueCreate(SDCARD_TASK_QUEUE_LEN, sizeof(sdcard_dialog_msg_t));
        if (s_sdcard_queue == NULL) {
            ESP_LOGE(TAG, "create sdcard queue failed");
            return pdFAIL;
        }
    }

    return xTaskCreate(sdcard_task,
                       "sdcard_task",
                       SDCARD_TASK_STACK,
                       NULL,
                       SDCARD_TASK_PRIO,
                       &s_sdcard_task_handle);
}

esp_err_t sdcard_task_post_dialog(sdcard_dialog_role_t role, const char *text)
{
    sdcard_dialog_msg_t msg = {0};
    sdcard_dialog_msg_t dropped_msg;

    if ((text == NULL) || (text[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sdcard_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    sdcard_task_fill_timestamp(&msg);
    msg.role = role;
    snprintf(msg.text, sizeof(msg.text), "%s", text);

    if (xQueueSend(s_sdcard_queue, &msg, 0) == pdPASS) {
        return ESP_OK;
    }

    if (xQueueReceive(s_sdcard_queue, &dropped_msg, 0) == pdTRUE) {
        if (xQueueSend(s_sdcard_queue, &msg, 0) == pdPASS) {
            ESP_LOGW(TAG, "sdcard queue full, dropped oldest dialog");
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

bool sdcard_task_is_ready(void)
{
    return s_sdcard_task_ready && tf_sdcard_is_mounted();
}
