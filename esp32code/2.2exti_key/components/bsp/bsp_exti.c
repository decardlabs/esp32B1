#include "bsp_exti.h"

void gpio_isr_handler(void* arg);
void bsp_exti_init(void)
{
    gpio_config_t key_config = {
        .pin_bit_mask = (1ULL<<KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config(&key_config);

    gpio_install_isr_service(0); 

    gpio_isr_handler_add(KEY_GPIO, gpio_isr_handler, NULL);

}

void gpio_isr_handler(void* arg)
{
    static uint64_t last_time = 0; 
    uint64_t now = esp_timer_get_time();
    if (now - last_time < 20000) return;
    last_time = now;

    bsp_gpio_toggle_level(LED_GPIO);
}
