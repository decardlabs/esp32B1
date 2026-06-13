#include "task_led.h"

static const char *TAG = "TASK_LED"; 

TaskHandle_t led_task_handle = NULL;
// struct tskTaskControlBlock * led_task_handle = NULL;    //int *a = 0;
// struct tskTaskControlBlock  led_task_handle = NULL;   //  int a =0;

StackType_t led_static_task_buffer[2048*2];
StaticTask_t led_static_task_tcb;
TaskHandle_t led_static_task_handle = NULL;
QueueHandle_t gptimer_led_queue = NULL;

void led_task(void *pvParameters)
{
    bool led_status = 0;
    TaskHandle_t handle;
    gptimer_led_queue = xQueueCreate(10, sizeof(uint32_t));
    bsp_gptimer_init(gptimer_led_queue);
    while (1)
    {
        uint32_t led_count = 0;
        xQueueReceive(gptimer_led_queue, &led_count, portMAX_DELAY);
        led_status = led_count % 2;
        gpio_set_level(LED_GPIO, led_status);
        ESP_LOGI(TAG,"led_count = %d, led_status = %d\n", led_count, led_status);
    }
    ESP_LOGI(TAG,"led_task end\n");
    vTaskDelete(NULL);
}


TaskHandle_t led_static_task_create(void)
{
    //xTaskCreate(led_task, "led_task",2048*2, NULL, 10, &led_task_handle);
    //xTaskCreatePinnedToCore(led_task, "led_task",2048*2, NULL, 10, &led_task_handle, 1);
    led_static_task_handle = xTaskCreateStatic(led_task, "led_task",2048*2, NULL, 10, led_static_task_buffer, &led_static_task_tcb);
    return led_static_task_handle;
}

BaseType_t led_task_create(void)
{
    BaseType_t ret = xTaskCreate(led_task, "led_task",2048*2, NULL, 10, &led_task_handle);
    //xTaskCreatePinnedToCore(led_task, "led_task",2048*2, NULL, 10, &led_task_handle, 1);
    return ret;
}
