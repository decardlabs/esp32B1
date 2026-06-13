#include "ws2812b.h"
#include "xl9555.h"

static const char *TAG = "ws2812b";
led_strip_handle_t led_strip_handle = NULL;

led_strip_handle_t ws2812_init(void)
{
    ESP_ERROR_CHECK(xl9555_ws2812_level_shifter_enable());

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO_PIN,
        .max_leds = LED_STRIP_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        }
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED_STRIP_RMT_RES_HZ,
        .mem_block_symbols = LED_STRIP_MEMORY_BLOCK_WORDS,
        .flags = {
            .with_dma = LED_STRIP_USE_DMA,
        }
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_handle));
    ESP_ERROR_CHECK(led_strip_clear(led_strip_handle));

    return led_strip_handle;
}

esp_err_t ws2812_set_all(uint8_t r, uint8_t g, uint8_t b)
{
    if (led_strip_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(led_strip_handle, i, r, g, b), TAG, "set_pixel failed");
    }
    return led_strip_refresh(led_strip_handle);
}

esp_err_t ws2812_clear(void)
{
    if (led_strip_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(led_strip_clear(led_strip_handle), TAG, "clear failed");
    return led_strip_refresh(led_strip_handle);
}
