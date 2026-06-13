#include "task_timer.h"

static const char *TAG = "TASK_TIMER";
esp_timer_handle_t esptimer_handle = NULL;

void esptimer_callback(void *arg)
{
    ESP_LOGW(TAG, "esptimer_callback is called...");
}

void esptimer_init(void)
{
    esp_timer_create_args_t esptimer_args = {
        .callback = esptimer_callback,
        .name = "timer_task",
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    esp_timer_create(&esptimer_args, &esptimer_handle);
    esp_timer_start_periodic(esptimer_handle, 1000000*5);
}

void freertos_timer_cb(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "freertos_timer_cb is called");
}

void freertos_timer_init(void)
{
    TimerHandle_t xTimer = xTimerCreate("timer_task", pdMS_TO_TICKS(1000), pdTRUE, NULL, freertos_timer_cb);
    if(xTimer == NULL)
    {
        ESP_LOGE(TAG, "xTimerCreate failed");
    }
    xTimerStart(xTimer, 0);
}

void timer_task(void *pvParameters)
{
    esptimer_init();
    freertos_timer_init();
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


BaseType_t timer_task_create(void)
{
   return xTaskCreate(timer_task, "timer_task", 4096, NULL, 10, NULL);
}