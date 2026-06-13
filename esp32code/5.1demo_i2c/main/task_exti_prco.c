#include "task_exti_prco.h"

static const char *TAG = "TASK_EXTI_PRCO"; 

TaskHandle_t exti_task_prco_handle = NULL; 

void exti_task_prco(void *pvParameters)
{
    uint32_t notify_value = 0;
    bsp_exti_init(KEY_GPIO);
    bsp_exti_init(BOOT_GPIO);
    bsp_exti_register_task_handle(exti_task_prco_handle);
    while (1)
    {    
        xTaskNotifyWait(0, ULONG_MAX, &notify_value, portMAX_DELAY);
        if(notify_value == KEY_GPIO)
        {
            bsp_gpio_toggle_level(LED_GPIO);
            ESP_LOGI(TAG, "key gpio notify");
        }
        else if(notify_value == BOOT_GPIO)
        {
            bsp_gpio_toggle_level(LED_GPIO);
            ESP_LOGI(TAG,"boot gpio notify");
        }
    }
}

BaseType_t exti_task_prco_create(void)
{
    BaseType_t ret = xTaskCreate(exti_task_prco, "exti_task_prco",4096, NULL, 10, &exti_task_prco_handle);
    return ret;
}
