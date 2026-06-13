#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "store/config/ble_store_config.h"

#include "bsp_i2c.h"
#include "bsp_spi.h"
#include "xl9555.h"
#include "lcd_st7796.h"

void ble_store_config_init(void);

#define TAG "HID_KBD"
#define KEY_SAMPLE_MS 20
#define DEBOUNCE_MS 30
#define UI_REFRESH_MS 250
#define BEEP_ON_MS 80

#define KEY0_GPIO GPIO_NUM_40
#define KEY_BOOT_GPIO GPIO_NUM_0

#define UI_W LCD_ST7796_H_RES
#define UI_H 160
#define UI_Y_OFFSET ((LCD_ST7796_V_RES - UI_H) / 2)

#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))

/* USB HID keyboard keycodes */
#define HID_KEY_1 0x1E
#define HID_KEY_0 0x27

static TaskHandle_t s_key_task = NULL;
static TaskHandle_t s_ui_task = NULL;
static esp_hidd_dev_t *s_hid_dev = NULL;
static bool s_next_zero = true;
static bool s_hid_connected = false;
static bool s_hid_secured = false;
static bool s_beep_ready = false;
static bool s_lcd_ready = false;
static uint32_t s_key_count = 0;
static char s_last_key = '-';
static uint16_t *s_ui_buf = NULL;

static bool hid_link_ready(bool *api_connected_out)
{
    bool api_connected = (s_hid_dev != NULL) && esp_hidd_dev_connected(s_hid_dev);
    if (api_connected_out != NULL) {
        *api_connected_out = api_connected;
    }
    return s_hid_connected || api_connected;
}

static void lcd_clear_fullscreen(uint16_t color)
{
    if (!s_lcd_ready) {
        return;
    }

    const uint16_t chunk_lines = LCD_ST7796_BUF_LINES;
    uint16_t *chunk = (uint16_t *)malloc((size_t)UI_W * chunk_lines * sizeof(uint16_t));
    if (chunk == NULL) {
        ESP_LOGW(TAG, "fullscreen clear skipped: no mem");
        return;
    }

    for (size_t i = 0; i < (size_t)UI_W * chunk_lines; ++i) {
        chunk[i] = color;
    }

    for (uint16_t y = 0; y < LCD_ST7796_V_RES; y += chunk_lines) {
        uint16_t h = (uint16_t)((y + chunk_lines <= LCD_ST7796_V_RES) ? chunk_lines : (LCD_ST7796_V_RES - y));
        (void)lcd_st7796_draw_bitmap(0, y, UI_W, y + h, chunk);
    }

    free(chunk);
}

typedef struct {
    bool last_raw;
    bool stable_state;
    uint32_t stable_ms;
} key_filter_t;

typedef struct {
    gpio_num_t gpio;
    const char *name;
    key_filter_t filter;
} key_entry_t;

static key_entry_t s_keys[] = {
    {.gpio = KEY0_GPIO, .name = "KEY0"},
    {.gpio = KEY_BOOT_GPIO, .name = "BOOT"},
};

static const uint8_t s_keyboard_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x05,
    0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {.data = s_keyboard_report_map, .len = sizeof(s_keyboard_report_map)},
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0100,
    .device_name = "ESP32S3-HID-KBD",
    .manufacturer_name = "Espressif",
    .serial_number = "test006-kbd",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static void hid_send_key(uint8_t keycode)
{
    if (s_hid_dev == NULL || !esp_hidd_dev_connected(s_hid_dev)) {
        return;
    }

    uint8_t report[8] = {0};
    report[2] = keycode;
    esp_hidd_dev_input_set(s_hid_dev, 0, 1, report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(20));
    memset(report, 0, sizeof(report));
    esp_hidd_dev_input_set(s_hid_dev, 0, 1, report, sizeof(report));
}

static void beep_click(void)
{
    if (!s_beep_ready) {
        return;
    }
    (void)xl9555_beep_set(true);
    vTaskDelay(pdMS_TO_TICKS(BEEP_ON_MS));
    (void)xl9555_beep_set(false);
}

static bool key_filter_update(key_filter_t *filter, bool raw_pressed)
{
    if (raw_pressed != filter->last_raw) {
        filter->last_raw = raw_pressed;
        filter->stable_ms = 0;
        return false;
    }

    filter->stable_ms += KEY_SAMPLE_MS;
    if ((filter->stable_ms >= DEBOUNCE_MS) && (raw_pressed != filter->stable_state)) {
        filter->stable_state = raw_pressed;
        return filter->stable_state;
    }

    return false;
}

static void render_fill(uint16_t color)
{
    for (size_t i = 0; i < (size_t)UI_W * UI_H; ++i) {
        s_ui_buf[i] = color;
    }
}

static inline void draw_px(int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= UI_W || y >= UI_H) {
        return;
    }
    s_ui_buf[y * UI_W + x] = color;
}

static const uint8_t *glyph5x7(char c)
{
    static const uint8_t BLANK[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t G_0[7] = {14, 17, 19, 21, 25, 17, 14};
    static const uint8_t G_1[7] = {4, 12, 4, 4, 4, 4, 14};
    static const uint8_t G_2[7] = {14, 17, 1, 2, 4, 8, 31};
    static const uint8_t G_3[7] = {30, 1, 1, 14, 1, 1, 30};
    static const uint8_t G_4[7] = {2, 6, 10, 18, 31, 2, 2};
    static const uint8_t G_5[7] = {31, 16, 16, 30, 1, 1, 30};
    static const uint8_t G_6[7] = {14, 16, 16, 30, 17, 17, 14};
    static const uint8_t G_7[7] = {31, 1, 2, 4, 8, 8, 8};
    static const uint8_t G_8[7] = {14, 17, 17, 14, 17, 17, 14};
    static const uint8_t G_9[7] = {14, 17, 17, 15, 1, 1, 14};
    static const uint8_t G_A[7] = {14, 17, 17, 31, 17, 17, 17};
    static const uint8_t G_B[7] = {30, 17, 17, 30, 17, 17, 30};
    static const uint8_t G_C[7] = {14, 17, 16, 16, 16, 17, 14};
    static const uint8_t G_D[7] = {28, 18, 17, 17, 17, 18, 28};
    static const uint8_t G_E[7] = {31, 16, 16, 30, 16, 16, 31};
    static const uint8_t G_H[7] = {17, 17, 17, 31, 17, 17, 17};
    static const uint8_t G_I[7] = {14, 4, 4, 4, 4, 4, 14};
    static const uint8_t G_K[7] = {17, 18, 20, 24, 20, 18, 17};
    static const uint8_t G_L[7] = {16, 16, 16, 16, 16, 16, 31};
    static const uint8_t G_N[7] = {17, 25, 21, 19, 17, 17, 17};
    static const uint8_t G_O[7] = {14, 17, 17, 17, 17, 17, 14};
    static const uint8_t G_P[7] = {30, 17, 17, 30, 16, 16, 16};
    static const uint8_t G_R[7] = {30, 17, 17, 30, 20, 18, 17};
    static const uint8_t G_S[7] = {15, 16, 16, 14, 1, 1, 30};
    static const uint8_t G_T[7] = {31, 4, 4, 4, 4, 4, 4};
    static const uint8_t G_U[7] = {17, 17, 17, 17, 17, 17, 14};
    static const uint8_t G_V[7] = {17, 17, 17, 17, 17, 10, 4};
    static const uint8_t G_Y[7] = {17, 17, 10, 4, 4, 4, 4};
    static const uint8_t G_COLON[7] = {0, 4, 4, 0, 4, 4, 0};
    static const uint8_t G_DASH[7] = {0, 0, 0, 31, 0, 0, 0};

    switch (c) {
    case '0': return G_0;
    case '1': return G_1;
    case '2': return G_2;
    case '3': return G_3;
    case '4': return G_4;
    case '5': return G_5;
    case '6': return G_6;
    case '7': return G_7;
    case '8': return G_8;
    case '9': return G_9;
    case 'A': return G_A;
    case 'B': return G_B;
    case 'C': return G_C;
    case 'D': return G_D;
    case 'E': return G_E;
    case 'H': return G_H;
    case 'I': return G_I;
    case 'K': return G_K;
    case 'L': return G_L;
    case 'N': return G_N;
    case 'O': return G_O;
    case 'P': return G_P;
    case 'R': return G_R;
    case 'S': return G_S;
    case 'T': return G_T;
    case 'U': return G_U;
    case 'V': return G_V;
    case 'Y': return G_Y;
    case ':': return G_COLON;
    case '-': return G_DASH;
    case ' ': return BLANK;
    default: return BLANK;
    }
}

static void draw_char5x7(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    const uint8_t *g = glyph5x7(c);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            bool on = ((g[row] >> (4 - col)) & 0x01) != 0;
            uint16_t color = on ? fg : bg;
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    draw_px(x + col * scale + sx, y + row * scale + sy, color);
                }
            }
        }
    }
}

static void draw_text5x7(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    while (*text) {
        draw_char5x7(x, y, *text, fg, bg, scale);
        x += 6 * scale;
        ++text;
    }
}

static void render_status_screen(void)
{
    char line[48];
    const uint16_t bg = RGB565(0, 0, 255);
    const uint16_t txt = RGB565(255, 255, 255);
    const int x0 = 16;
    const int scale = 2;
    const int line_h = 7 * scale;
    const int line_gap = 8;
    const int lines = 6;
    int y = (UI_H - (lines * line_h + (lines - 1) * line_gap)) / 2;

    render_fill(bg);
    draw_text5x7(x0, y, "BLE HID STATUS", txt, bg, scale);
    y += line_h + line_gap;

    snprintf(line, sizeof(line), "BT: %s", s_hid_connected ? "CONNECTED" : "ADVERTISING");
    draw_text5x7(x0, y, line, txt, bg, scale);
    y += line_h + line_gap;

    snprintf(line, sizeof(line), "SECURITY: %s", s_hid_secured ? "ENCRYPTED" : "OPEN");
    draw_text5x7(x0, y, line, txt, bg, scale);
    y += line_h + line_gap;

    snprintf(line, sizeof(line), "LAST KEY: %c", s_last_key);
    draw_text5x7(x0, y, line, txt, bg, scale);
    y += line_h + line_gap;

    snprintf(line, sizeof(line), "KEY COUNT: %lu", (unsigned long)s_key_count);
    draw_text5x7(x0, y, line, txt, bg, scale);
    y += line_h + line_gap;

    snprintf(line, sizeof(line), "BEEP: %s", s_beep_ready ? "READY" : "OFFLINE");
    draw_text5x7(x0, y, line, txt, bg, scale);
}

static void ui_task(void *arg)
{
    (void)arg;
    while (1) {
        render_status_screen();
        if (s_lcd_ready && s_ui_buf != NULL) {
            (void)lcd_st7796_draw_bitmap(0, UI_Y_OFFSET, UI_W, UI_Y_OFFSET + UI_H, s_ui_buf);
        }
        vTaskDelay(pdMS_TO_TICKS(UI_REFRESH_MS));
    }
}

static void key_task(void *arg)
{
    (void)arg;

    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); ++i) {
        bool raw_pressed = (gpio_get_level(s_keys[i].gpio) == 0);
        s_keys[i].filter.last_raw = raw_pressed;
        s_keys[i].filter.stable_state = raw_pressed;
        s_keys[i].filter.stable_ms = 0;
    }

    while (1) {
        for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); ++i) {
            bool raw_pressed = (gpio_get_level(s_keys[i].gpio) == 0);
            bool pressed_edge = key_filter_update(&s_keys[i].filter, raw_pressed);
            if (!pressed_edge) {
                continue;
            }

            uint8_t keycode = s_next_zero ? HID_KEY_0 : HID_KEY_1;
            s_last_key = s_next_zero ? '0' : '1';
            s_next_zero = !s_next_zero;
            s_key_count++;

            bool api_connected = false;
            if (hid_link_ready(&api_connected)) {
                ESP_LOGI(TAG, "%s pressed -> send '%c'", s_keys[i].name, s_last_key);
                hid_send_key(keycode);
            } else {
                ESP_LOGW(TAG,
                         "%s pressed but host not connected (evt=%d api=%d sec=%d dev=%d)",
                         s_keys[i].name,
                         (int)s_hid_connected,
                         (int)api_connected,
                         (int)s_hid_secured,
                         (int)(s_hid_dev != NULL));
            }

            beep_click();
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_SAMPLE_MS));
    }
}

static void key_task_start(void)
{
    if (s_key_task == NULL) {
        BaseType_t ok = xTaskCreate(key_task, "hid_key_task", 4096, NULL, 5, &s_key_task);
        if (ok != pdPASS) {
            s_key_task = NULL;
            ESP_LOGE(TAG, "failed to create key task");
        }
    }
}

void ble_hid_task_start_up(void)
{
    key_task_start();
}

void hid_on_security_changed(bool secure)
{
    s_hid_secured = secure;
}

void hid_on_disconnected(void)
{
    s_hid_connected = false;
    s_hid_secured = false;
}

static void ble_hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID start -> advertising");
        s_hid_connected = false;
        s_hid_secured = false;
        esp_hid_ble_gap_adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID connected");
        s_hid_connected = true;
        key_task_start();
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "HID disconnected");
        hid_on_disconnected();
        esp_hid_ble_gap_adv_start();
        break;
    default:
        break;
    }
}

static void ble_hid_device_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void key_gpio_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << KEY0_GPIO) | (1ULL << KEY_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));
}

static void platform_drivers_init(void)
{
    esp_err_t ret;

    bsp_i2c0_init();

    ret = xl9555_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "xl9555_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = bsp_spi2_lcd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_spi2_lcd_init failed: %s", esp_err_to_name(ret));
        return;
    }

    if (lcd_st7796_init() == ESP_OK) {
        s_lcd_ready = true;
    } else {
        ESP_LOGW(TAG, "lcd_st7796_init failed, continue without TFT");
    }

    if (xl9555_audio_gpio_init() == ESP_OK) {
        s_beep_ready = true;
    } else {
        s_beep_ready = false;
        ESP_LOGW(TAG, "xl9555_audio_gpio_init failed, beep disabled");
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    key_gpio_init();

    s_ui_buf = (uint16_t *)malloc(UI_W * UI_H * sizeof(uint16_t));
    if (s_ui_buf == NULL) {
        ESP_LOGE(TAG, "ui buffer alloc failed");
    }

    platform_drivers_init();

    if (s_lcd_ready) {
        lcd_clear_fullscreen(RGB565(0, 0, 255));
    }

    if (s_lcd_ready && s_ui_buf != NULL && s_ui_task == NULL) {
        xTaskCreate(ui_task, "status_ui_task", 6144, NULL, 4, &s_ui_task);
    }

    ESP_LOGI(TAG, "Init HID GAP");
    ESP_ERROR_CHECK(esp_hid_gap_init(HID_DEV_MODE));

    ESP_LOGI(TAG, "Init BLE HID advertising payload");
    ESP_ERROR_CHECK(esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, s_hid_config.device_name));

    ESP_LOGI(TAG, "Init BLE HID device");
    ESP_ERROR_CHECK(esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, ble_hidd_event_callback, &s_hid_dev));

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ESP_LOGI(TAG, "Enable NimBLE host");
    ESP_ERROR_CHECK(esp_nimble_enable(ble_hid_device_host_task));

    key_task_start();

    ESP_LOGI(TAG, "Ready. Pair from phone/computer with device: %s", s_hid_config.device_name);
    ESP_LOGI(TAG, "After connected, press KEY0(GPIO40) or BOOT(GPIO0) to type 0/1 alternately.");
}
