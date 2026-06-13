#include "task_timer.h"

static const char *TAG = "TASK_TIMER";
esp_timer_handle_t esptimer_handle = NULL;  




void freertos_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGW(TAG,"freertos_timer_callback running.........\n");
}

void esptimer_callback(void *arg)
{
    ESP_LOGI(TAG,"esptimer_callback running\n");
}

void freertos_timer_init(void)
{
     TimerHandle_t xTimer = xTimerCreate("freertos_timer", 1000 / portTICK_PERIOD_MS, pdTRUE, NULL, freertos_timer_callback);

    xTimerStart(xTimer, 10);
}



void esptimer_init(void)
{
    esp_timer_create_args_t esptimer_args = {
        .callback = esptimer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "esptimer_task",
    };
    esp_timer_create(&esptimer_args, &esptimer_handle);

    esp_timer_start_periodic(esptimer_handle, 5000000);
}


void timer_task(void *pvParameters)
{
    esptimer_init();
    freertos_timer_init();
    while (1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);

    }
}

BaseType_t timer_task_create(void)
{
    BaseType_t ret = xTaskCreate(timer_task, "timer_task",4096, NULL, 10, NULL);
    return ret;
}

