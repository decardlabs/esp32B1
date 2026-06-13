#include "bsp_exti.h"

// 声明外部中断服务例程处理函数
static void exti_isr_handler(void* arg);

// 用于保存外部中断处理任务句柄的静态变量
static TaskHandle_t reg_exti_task_handle = NULL;

/**
 * @brief 初始化指定GPIO引脚的外部中断功能
 * 
 * 配置指定GPIO引脚为输入模式，启用上拉电阻，并设置为下降沿触发中断。
 * 同时初始化GPIO中断服务并注册中断处理函数。
 * 
 * @param gpio_num 要配置为外部中断的GPIO引脚号
 */
void bsp_exti_init(gpio_num_t gpio_num)
{
    // 配置按键引脚为输入，带上拉电阻（按键另一端接地）
    gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << gpio_num),     // 设置引脚位掩码
        .mode = GPIO_MODE_INPUT,                // 设置为输入模式
        .pull_up_en = GPIO_PULLUP_ENABLE,       // 启用上拉电阻
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  // 禁用下拉电阻
        .intr_type = GPIO_INTR_NEGEDGE          // 设置为下降沿触发中断（按键按下时）
    };
    gpio_config(&button_config);

    // 确保只安装一次GPIO中断服务
    static bool isr_service_installed = false;
    if (!isr_service_installed) {
        gpio_install_isr_service(0); // 0 表示使用默认优先级
        isr_service_installed = true;
    }

    // 注册按键中断处理函数
    gpio_isr_handler_add(gpio_num, exti_isr_handler, (void*)gpio_num);
}

/**
 * @brief 外部中断服务例程处理函数
 * 
 * 当GPIO引脚发生中断时被调用，在ISR(中断服务例程)中执行。
 * 实现按键防抖功能，并向注册的任务发送通知。
 * 
 * @param arg 传递给中断处理函数的参数，这里是GPIO引脚号
 */
static void IRAM_ATTR exti_isr_handler(void* arg)
{
    // 记录上次触发时间，用于防抖
    static uint64_t last_time = 0;        
    // 获取当前时间（微秒）
    uint64_t now = esp_timer_get_time();  

    // 防抖处理：如果距离上次触发时间小于20ms，则忽略本次触发
    if (now - last_time < 20000) return;  
    // 更新上次触发时间为当前时间
    last_time = now;
    
    // 获取触发中断的GPIO引脚号
    gpio_num_t gpio_num = (gpio_num_t)arg;
    // 用于记录是否有更高优先级任务需要唤醒
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    xTaskNotifyFromISR(reg_exti_task_handle, gpio_num, eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
 
   // 如果有更高优先级任务被唤醒，则进行任务切换
    if(xHigherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

/**
 * @brief 注册外部中断处理任务的句柄
 * 
 * 将任务句柄保存到全局变量中，供中断服务例程使用，
 * 以便能够向正确的任务发送通知。
 * 
 * @param task_handle 外部中断处理任务的句柄
 */
void bsp_exti_register_task_handle(TaskHandle_t task_handle) {
    reg_exti_task_handle = task_handle;  
}