#include "task_exti.h"

// 外部中断处理任务的句柄，用于任务间通信和控制
TaskHandle_t exti_proc_task_handle = NULL;

/**
 * @brief 外部中断处理任务函数
 * 
 * 该任务负责处理来自GPIO外部中断的事件。当按键按下时，中断服务例程会发送任务通知
 * 给这个任务，任务接收到通知后根据GPIO引脚号执行相应的操作。
 * 
 * @param arg 任务参数指针（未使用）
 */
static void exti_proc_task(void *arg)
{
    // 存储接收到的通知值（GPIO引脚号）
    uint32_t notify_value = 0;  
    
    // 注册任务句柄到bsp层，便于中断服务例程访问
    bsp_exti_register_task_handle(exti_proc_task_handle);
    
    // 任务主循环
    while (1) {
        // 阻塞等待任务通知（无限等待）
        if (xTaskNotifyWait(
            0x00,               // 进入等待前不清除通知值
            ULONG_MAX,          // 退出等待后清除所有通知值
            &notify_value,      // 接收通知值（GPIO引脚号）
            portMAX_DELAY       // 无限等待
        ) == pdPASS) {

            // 根据通知值（GPIO引脚号）区分按键
            switch ((gpio_num_t)notify_value) 
            {
                case KEY_GPIO:
                    // 切换LED状态（开<->关）
                    bsp_gpio_toggle(LED_GPIO);
                    // 打印按键1按下的信息
                    printf("KEY0 pressed\n");
                    break;
                case BOOT_GPIO:
                    // 处理按键2的任务
                    // 打印BOOT按键按下的信息
                    printf("BOOT pressed\n");
                    break;
                default:
                    // 未知GPIO引脚号，不处理
                    break;
            }
        }
    }
}

/**
 * @brief 创建外部中断处理任务
 * 
 * 使用FreeRTOS的xTaskCreate函数创建一个处理外部中断的任务。
 * 
 * @return BaseType_t 任务创建结果，pdPASS表示成功，其他值表示失败
 */
BaseType_t exti_proc_task_create(void)
{
    // 创建任务：任务函数、任务名称、堆栈大小、任务参数、优先级、任务句柄
    BaseType_t ret = xTaskCreate(
        exti_proc_task,         // 任务函数
        "exti_proc_task",       // 任务名称
        2048*2,                 // 堆栈大小（字节）
        NULL,                   // 任务参数
        10,                     // 任务优先级
        &exti_proc_task_handle  // 任务句柄
    );

    return ret;
}