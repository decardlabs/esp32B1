#include "lcd_st7796.h"
#include <string.h>
#include "bsp_spi.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xl9555.h"

#define LCD_PIXEL_CLOCK_HZ          (40 * 1000 * 1000)
#define LCD_CMD_BITS                8
#define LCD_PARAM_BITS              8
#define LCD_POST_DISPON_DELAY_MS    120

#define LCD_PIN_NUM_DC              BSP_GPIO_LCD_DC
#define LCD_PIN_NUM_CS              BSP_GPIO_LCD_CS
#define LCD_PIN_NUM_TOUCH_CS        BSP_GPIO_TOUCH_CS

static const char *TAG = "LCD_ST7796";

static esp_lcd_panel_io_handle_t lcd_io_handle = NULL;
static bool lcd_panel_ready = false;
static esp_lcd_panel_io_callbacks_t s_lcd_io_callbacks = {0};
static void *s_lcd_io_user_ctx = NULL;
static volatile lcd_st7796_transfer_t s_pending_transfer = LCD_ST7796_TRANSFER_NONE;
static volatile lcd_st7796_transfer_t s_completed_transfer = LCD_ST7796_TRANSFER_NONE;

static bool lcd_st7796_color_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_io_event_data_t *edata,
                                     void *user_ctx)
{
    lcd_st7796_transfer_t transfer = s_pending_transfer;
    bool need_yield = bsp_spi2_bus_unlock_from_isr();

    (void)user_ctx;

    s_pending_transfer = LCD_ST7796_TRANSFER_NONE;
    s_completed_transfer = transfer;

    if (s_lcd_io_callbacks.on_color_trans_done != NULL) {
        need_yield = s_lcd_io_callbacks.on_color_trans_done(panel_io, edata, s_lcd_io_user_ctx) || need_yield;
    }

    return need_yield;
}

lcd_st7796_transfer_t lcd_st7796_take_completed_transfer_from_isr(void)
{
    lcd_st7796_transfer_t transfer = s_completed_transfer;

    s_completed_transfer = LCD_ST7796_TRANSFER_NONE;
    return transfer;
}

static esp_err_t lcd_st7796_write_cmd(uint8_t cmd)
{
    ESP_RETURN_ON_FALSE(lcd_io_handle, ESP_ERR_INVALID_STATE, TAG, "lcd io not ready");
    return esp_lcd_panel_io_tx_param(lcd_io_handle, cmd, NULL, 0);
}

static esp_err_t lcd_st7796_write_param(uint8_t cmd, const void *param, size_t param_size)
{
    ESP_RETURN_ON_FALSE(lcd_io_handle, ESP_ERR_INVALID_STATE, TAG, "lcd io not ready");
    return esp_lcd_panel_io_tx_param(lcd_io_handle, cmd, param, param_size);
}

static void lcd_st7796_ctrl_gpio_init(void)
{
    gpio_config_t touch_cs_cfg = {
        .pin_bit_mask = 1ULL << LCD_PIN_NUM_TOUCH_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&touch_cs_cfg);
    gpio_set_level(LCD_PIN_NUM_TOUCH_CS, 1);
}

static esp_err_t lcd_st7796_bus_init(void)
{
    esp_err_t ret;

    if (lcd_io_handle) {
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(bsp_spi2_lcd_is_initialized(), ESP_ERR_INVALID_STATE, TAG, "call bsp_spi2_lcd_init() first");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_NUM_DC,
        .cs_gpio_num = LCD_PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .cs_ena_pretrans = 2,
        .cs_ena_posttrans = 2,
        .on_color_trans_done = lcd_st7796_color_done_cb,
    };

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)bsp_get_spi2_lcd_host(), &io_cfg, &lcd_io_handle);
    return ret;
}

static esp_err_t lcd_st7796_panel_init(void)
{
    const uint8_t cmd_f0_c3 = 0xC3;
    const uint8_t cmd_f0_96 = 0x96;
    // Portrait mode, same MADCTL value as STM32 driver USE_HORIZONTAL == 0.
    const uint8_t cmd_36 = 0x48;
    const uint8_t cmd_3a = 0x05;
    const uint8_t cmd_e8[] = {0x40, 0x82, 0x07, 0x18, 0x27, 0x0A, 0xB6, 0x33};
    const uint8_t cmd_c5 = 0x27;
    const uint8_t cmd_c2 = 0xA7;
    const uint8_t cmd_e0[] = {0xF0, 0x01, 0x06, 0x0F, 0x12, 0x1D, 0x36, 0x54, 0x44, 0x0C, 0x18, 0x16, 0x13, 0x15};
    const uint8_t cmd_e1[] = {0xF0, 0x01, 0x05, 0x0A, 0x0B, 0x07, 0x32, 0x44, 0x44, 0x0C, 0x18, 0x17, 0x13, 0x16};
    const uint8_t cmd_f0_3c = 0x3C;
    const uint8_t cmd_f0_69 = 0x69;

    ESP_RETURN_ON_ERROR(lcd_st7796_write_cmd(LCD_CMD_SLPOUT), TAG, "sleep out failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(0xF0, &cmd_f0_c3, 1), TAG, "write F0 C3 failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(0xF0, &cmd_f0_96, 1), TAG, "write F0 96 failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(LCD_CMD_MADCTL, &cmd_36, 1), TAG, "write MADCTL failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(LCD_CMD_COLMOD, &cmd_3a, 1), TAG, "write COLMOD failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(0xE8, cmd_e8, sizeof(cmd_e8)), TAG, "write E8 failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(0xC5, &cmd_c5, 1), TAG, "write C5 failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(0xC2, &cmd_c2, 1), TAG, "write C2 failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(0xE0, cmd_e0, sizeof(cmd_e0)), TAG, "write E0 failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(0xE1, cmd_e1, sizeof(cmd_e1)), TAG, "write E1 failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(0xF0, &cmd_f0_3c, 1), TAG, "write F0 3C failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_param(0xF0, &cmd_f0_69, 1), TAG, "write F0 69 failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_write_cmd(LCD_CMD_DISPON), TAG, "display on failed");
    vTaskDelay(pdMS_TO_TICKS(LCD_POST_DISPON_DELAY_MS));

    return ESP_OK;
}

esp_err_t lcd_st7796_init(void)
{
    if (lcd_panel_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(xl9555_lcd_gpio_init(), TAG, "lcd gpio init failed");
    lcd_st7796_ctrl_gpio_init();
    ESP_RETURN_ON_ERROR(lcd_st7796_bus_init(), TAG, "lcd bus init failed");

    ESP_RETURN_ON_ERROR(xl9555_lcd_reset(), TAG, "lcd reset failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_panel_init(), TAG, "lcd panel init failed");
    ESP_RETURN_ON_ERROR(lcd_st7796_backlight_on(), TAG, "lcd backlight on failed");

    lcd_panel_ready = true;
    ESP_LOGI(TAG, "LCD init done");
    return ESP_OK;
}

esp_err_t lcd_st7796_register_event_callbacks(const esp_lcd_panel_io_callbacks_t *callbacks, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(lcd_io_handle, ESP_ERR_INVALID_STATE, TAG, "lcd io not ready");

    if (callbacks != NULL) {
        s_lcd_io_callbacks = *callbacks;
    } else {
        memset(&s_lcd_io_callbacks, 0, sizeof(s_lcd_io_callbacks));
    }
    s_lcd_io_user_ctx = user_ctx;

    return ESP_OK;
}

esp_err_t lcd_st7796_draw_bitmap_owned(uint16_t x_start,
                                       uint16_t y_start,
                                       uint16_t x_end,
                                       uint16_t y_end,
                                       const void *color_data,
                                       lcd_st7796_transfer_t transfer_type)
{
    uint8_t x_param[4] = {
        (uint8_t)(x_start >> 8),
        (uint8_t)(x_start & 0xFF),
        (uint8_t)((x_end - 1) >> 8),
        (uint8_t)((x_end - 1) & 0xFF),
    };
    uint8_t y_param[4] = {
        (uint8_t)(y_start >> 8),
        (uint8_t)(y_start & 0xFF),
        (uint8_t)((y_end - 1) >> 8),
        (uint8_t)((y_end - 1) & 0xFF),
    };
    size_t color_size = (size_t)(x_end - x_start) * (y_end - y_start) * sizeof(uint16_t);
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(lcd_panel_ready, ESP_ERR_INVALID_STATE, TAG, "lcd panel not ready");
    ESP_RETURN_ON_FALSE(color_data, ESP_ERR_INVALID_ARG, TAG, "color data is null");
    ESP_RETURN_ON_FALSE((x_start < x_end) && (y_start < y_end), ESP_ERR_INVALID_ARG, TAG, "invalid area");
    ESP_RETURN_ON_FALSE((x_end <= LCD_ST7796_H_RES) && (y_end <= LCD_ST7796_V_RES), ESP_ERR_INVALID_ARG, TAG, "area out of screen");

    ESP_RETURN_ON_ERROR(bsp_spi2_bus_lock(portMAX_DELAY), TAG, "spi2 bus lock failed");
    s_pending_transfer = transfer_type;

    ret = esp_lcd_panel_io_tx_param(lcd_io_handle, LCD_CMD_CASET, x_param, sizeof(x_param));
    if (ret != ESP_OK) {
        s_pending_transfer = LCD_ST7796_TRANSFER_NONE;
        bsp_spi2_bus_unlock();
        return ret;
    }

    ret = esp_lcd_panel_io_tx_param(lcd_io_handle, LCD_CMD_RASET, y_param, sizeof(y_param));
    if (ret != ESP_OK) {
        s_pending_transfer = LCD_ST7796_TRANSFER_NONE;
        bsp_spi2_bus_unlock();
        return ret;
    }

    ret = esp_lcd_panel_io_tx_color(lcd_io_handle, LCD_CMD_RAMWR, color_data, color_size);
    if (ret != ESP_OK) {
        s_pending_transfer = LCD_ST7796_TRANSFER_NONE;
        bsp_spi2_bus_unlock();
    }

    return ret;
}

esp_err_t lcd_st7796_draw_bitmap(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end, const void *color_data)
{
    return lcd_st7796_draw_bitmap_owned(x_start, y_start, x_end, y_end, color_data, LCD_ST7796_TRANSFER_NONE);
}

esp_err_t lcd_st7796_backlight_on(void)
{
    return xl9555_lcd_backlight_on();
}

esp_err_t lcd_st7796_backlight_off(void)
{
    return xl9555_lcd_backlight_off();
}

bool lcd_st7796_is_initialized(void)
{
    return lcd_panel_ready;
}
