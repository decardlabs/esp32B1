#include "bsp_gpio.h"


void bsp_led_init(void)
{
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL<<LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_config);
    gpio_set_level(LED_GPIO,1);
}

void bsp_key_init(void)
{
    gpio_config_t key_config = {
        .pin_bit_mask = (1ULL<<KEY_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&key_config);
}

