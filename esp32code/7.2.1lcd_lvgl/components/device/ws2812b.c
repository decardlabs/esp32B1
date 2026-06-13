#include "ws2812b.h"
#include "xl9555.h"

led_strip_handle_t led_strip_handle = NULL;

static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// 简单 gamma（让低亮度更细腻，观感提升很大）
static uint8_t gamma8(uint8_t x) {
    // gamma ≈ 2.2 的近似：pow(x/255, 2.2)*255
    float f = (float)x / 255.0f;
    f = powf(f, 2.2f);
    return (uint8_t)(f * 255.0f + 0.5f);
}

// HSV -> RGB (h:0-359, s/v:0-255)
static void hsv2rgb(int h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    float hf = (float)(h % 360) / 60.0f;
    int i = (int)hf;
    float f = hf - i;

    float sv = (float)s / 255.0f;
    float vv = (float)v / 255.0f;

    float p = vv * (1.0f - sv);
    float q = vv * (1.0f - sv * f);
    float t = vv * (1.0f - sv * (1.0f - f));

    float rf=0,gf=0,bf=0;
    switch (i) {
        default:
        case 0: rf=vv; gf=t;  bf=p;  break;
        case 1: rf=q;  gf=vv; bf=p;  break;
        case 2: rf=p;  gf=vv; bf=t;  break;
        case 3: rf=p;  gf=q;  bf=vv; break;
        case 4: rf=t;  gf=p;  bf=vv; break;
        case 5: rf=vv; gf=p;  bf=q;  break;
    }
    *r = (uint8_t)(rf * 255.0f + 0.5f);
    *g = (uint8_t)(gf * 255.0f + 0.5f);
    *b = (uint8_t)(bf * 255.0f + 0.5f);
}

// 设置一个像素（带全局亮度 + gamma）
static void set_pixel_gamma(int i, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness /*0-255*/) {
    int rr = (r * brightness) / 255;
    int gg = (g * brightness) / 255;
    int bb = (b * brightness) / 255;

    uint8_t gr = gamma8(clamp_u8(rr));
    uint8_t gg2 = gamma8(clamp_u8(gg));
    uint8_t gb = gamma8(clamp_u8(bb));

    ESP_ERROR_CHECK(led_strip_set_pixel(led_strip_handle, i, gr, gg2, gb));
}

void effect_breathing(uint8_t r, uint8_t g, uint8_t b) 
{
    // 用正弦做呼吸，观感很顺
    for (int t = 0;; t = (t + 1) % 200) {
        float x = (float)t / 200.0f;                 // 0~1
        float s = (sinf(2.0f * 3.14159f * x - 1.5708f) + 1.0f) * 0.5f; // 0~1
        uint8_t br = (uint8_t)(s * 255.0f);

        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            set_pixel_gamma(i, r, g, b, br);
        }
        ESP_ERROR_CHECK(led_strip_refresh(led_strip_handle));
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void effect_rainbow(void) 
{
    int base_h = 0;
    while (1) {
        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            uint8_t r,g,b;
            int h = base_h + i * 30; // 每颗相位差
            hsv2rgb(h, 210, 120, &r, &g, &b);
            set_pixel_gamma(i, r, g, b, 120); // 亮度别太高，舒服
        }
        ESP_ERROR_CHECK(led_strip_refresh(led_strip_handle));
        base_h = (base_h + 2) % 360;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void effect_comet(void) 
{
    int head = 0;
    int dir = 1;
    while (1) {
        for (int i = 0; i < LED_STRIP_LED_COUNT; i++) {
            int d = abs(i - head);
            // 距离越远越暗：0 -> 255, 1 -> 120, 2 -> 50 ...
            uint8_t br = (d == 0) ? 255 : (d == 1) ? 120 : (d == 2) ? 50 : 10;

            // 彗星颜色：青色
            set_pixel_gamma(i, 20, 60, 40, br);
        }
        ESP_ERROR_CHECK(led_strip_refresh(led_strip_handle));

        head += dir;
        if (head == LED_STRIP_LED_COUNT - 1) dir = -1;
        else if (head == 0) dir = 1;

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}


led_strip_handle_t ws2812_init(void)
{
    ESP_ERROR_CHECK(xl9555_ws2812_level_shifter_enable());

     led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO_PIN, // The GPIO that connected to the LED strip's data line
        .max_leds = LED_STRIP_LED_COUNT,      // The number of LEDs in the strip,
        .led_model = LED_MODEL_WS2812,        // LED strip model
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // The color order of the strip: GRB
        .flags = {
            .invert_out = false, // don't invert the output signal
        }
    };

    // LED strip backend configuration: RMT
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,        // different clock source can lead to different power consumption
        .resolution_hz = LED_STRIP_RMT_RES_HZ, // RMT counter clock frequency
        .mem_block_symbols = LED_STRIP_MEMORY_BLOCK_WORDS, // the memory block size used by the RMT channel
        .flags = {
            .with_dma = LED_STRIP_USE_DMA,     // Using DMA can improve performance when driving more LEDs
        }
    };
    // Install LED strip driver according to the LED strip configuration
    led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_handle);
    
    return led_strip_handle;
}
