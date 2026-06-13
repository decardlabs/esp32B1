#include "task_led.h"





TaskHandle_t led_task_handle = NULL;
// struct tskTaskControlBlock * led_task_handle = NULL;    //int *a = 0;
// struct tskTaskControlBlock  led_task_handle = NULL;   //  int a =0;

StackType_t led_static_task_buffer[2048*2];
StaticTask_t led_static_task_tcb;
TaskHandle_t led_static_task_handle = NULL;


void led_task(void *pvParameters)
{
    bool led_status = 0;
    TaskHandle_t handle;
    // uint8_t count = 0;
    while (1)
    {
        bsp_gpio_set_level(LED_GPIO,led_status);
        led_status = !led_status;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        printf("task led_status: %d\n", led_status);  

        // 打印当前任务剩余栈空间（单位：Byte）
        UBaseType_t stack_remain = uxTaskGetStackHighWaterMark(NULL);
        printf("Remaining stack of led_task: %u Byte\n",stack_remain); 

        handle = xTaskGetCurrentTaskHandle();  // 获取当前任务句柄 
        printf("Current task handle = %p\n", handle);
        
        UBaseType_t priority = uxTaskPriorityGet(handle);
        printf("Current task priority = %u\n", priority);

        printf("Running on core: %d\n", xPortGetCoreID());   // 打印当前核心 ID
        // if(count++ >= 5)
        // {
        //     break;
        // }
    }
    printf("led_task end\n");
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
