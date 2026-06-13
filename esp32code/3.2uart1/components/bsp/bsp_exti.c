#include "bsp_exti.h"


TaskHandle_t reg_exti_task_handle = NULL;


static void IRAM_ATTR gpio_isr_handler(void* arg);
void bsp_exti_init(gpio_num_t gpio_num)
{
    gpio_config_t key_config = {
        .pin_bit_mask = (1ULL<<gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config(&key_config);

    static bool isr_service_installed = 0;
    if(0 == isr_service_installed) 
    gpio_install_isr_service(0); 
    isr_service_installed = 1;
    
    gpio_isr_handler_add(gpio_num, gpio_isr_handler, (void *)gpio_num);
    
}

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    static uint64_t last_time = 0; 
    uint64_t now = esp_timer_get_time();
    if (now - last_time < 20000) return;
    last_time = now;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    gpio_num_t gpio_num = (gpio_num_t)arg;

    xTaskNotifyFromISR(reg_exti_task_handle, gpio_num, eSetValueWithOverwrite, &xHigherPriorityTaskWoken);

    if(xHigherPriorityTaskWoken)
    {
           portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}


void bsp_exti_register_task_handle(TaskHandle_t task_handle)
{
    reg_exti_task_handle = task_handle;  
}
