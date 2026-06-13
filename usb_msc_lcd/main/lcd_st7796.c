#include "lcd_st7796.h"
#include "board_pins.h"
#include "font_8x16.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "LCD";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_xl9555_dev = NULL;
static spi_device_handle_t s_lcd_spi = NULL;

/* ── I2C + XL9555 helpers (transplanted from test003) ────────────────── */

static esp_err_t xl9555_write_reg(uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(s_xl9555_dev, buf, 2, portMAX_DELAY);
}

static esp_err_t xl9555_set_pin_level(uint8_t port, uint8_t pin, bool level)
{
    uint8_t reg = (port == 0) ? 0x02 : 0x03; /* OUTPUT_0 / OUTPUT_1 */
    uint8_t val;
    esp_err_t ret = i2c_master_transmit_receive(s_xl9555_dev, &reg, 1, &val, 1, portMAX_DELAY);
    if (ret != ESP_OK) return ret;
    if (level) val |=  (1 << pin);
    else       val &= ~(1 << pin);
    return xl9555_write_reg(reg, val);
}

static esp_err_t xl9555_set_pin_dir(uint8_t port, uint8_t pin, bool output)
{
    uint8_t reg = (port == 0) ? 0x06 : 0x07; /* CONFIG_0 / CONFIG_1 */
    uint8_t val;
    esp_err_t ret = i2c_master_transmit_receive(s_xl9555_dev, &reg, 1, &val, 1, portMAX_DELAY);
    if (ret != ESP_OK) return ret;
    if (output) val &= ~(1 << pin);
    else        val |=  (1 << pin);
    return xl9555_write_reg(reg, val);
}

static esp_err_t xl9555_output_init(uint8_t port, uint8_t pin, bool level)
{
    esp_err_t ret = xl9555_set_pin_dir(port, pin, true);
    if (ret != ESP_OK) return ret;
    return xl9555_set_pin_level(port, pin, level);
}

/* ── ST7796 SPI LCD control ─────────────────────────────────────────── */

static void lcd_spi_pre_transfer_cb(spi_transaction_t *t)
{
    gpio_set_level(PIN_LCD_DC, (int)(t->user));
}

static esp_err_t lcd_write_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void *)0,  /* DC low = command */
    };
    return spi_device_transmit(s_lcd_spi, &t);
}

static esp_err_t lcd_write_data(const void *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
        .user = (void *)1,  /* DC high = data */
    };
    return spi_device_transmit(s_lcd_spi, &t);
}

static esp_err_t lcd_write_reg(uint8_t cmd, const void *param, size_t param_len)
{
    esp_err_t ret = lcd_write_cmd(cmd);
    if (ret != ESP_OK) return ret;
    if (param && param_len) {
        ret = lcd_write_data(param, param_len);
    }
    return ret;
}

/* ── Initialization sequences ───────────────────────────────────────── */

static esp_err_t i2c_xl9555_init(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C0_SCL,
        .sda_io_num = PIN_I2C0_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus), TAG, "i2c0 init");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = XL9555_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_xl9555_dev),
                        TAG, "xl9555 add");

    return ESP_OK;
}

static esp_err_t lcd_panel_init(void)
{
    const uint8_t cmd_f0_c3 = 0xC3, cmd_f0_96 = 0x96;
    const uint8_t cmd_36 = 0x48;         /* MADCTL - portrait */
    const uint8_t cmd_3a = 0x05;         /* COLMOD - 16bit */
    const uint8_t cmd_e8[] = {0x40, 0x82, 0x07, 0x18, 0x27, 0x0A, 0xB6, 0x33};
    const uint8_t cmd_c5 = 0x27, cmd_c2 = 0xA7;
    const uint8_t cmd_e0[] = {0xF0, 0x01, 0x06, 0x0F, 0x12, 0x1D, 0x36, 0x54,
                               0x44, 0x0C, 0x18, 0x16, 0x13, 0x15};
    const uint8_t cmd_e1[] = {0xF0, 0x01, 0x05, 0x0A, 0x0B, 0x07, 0x32, 0x44,
                               0x44, 0x0C, 0x18, 0x17, 0x13, 0x16};
    const uint8_t cmd_f0_3c = 0x3C, cmd_f0_69 = 0x69;

    ESP_RETURN_ON_ERROR(lcd_write_cmd(0x11), TAG, "SLPOUT");     /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(lcd_write_reg(0xF0, &cmd_f0_c3, 1), TAG, "F0 C3");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0xF0, &cmd_f0_96, 1), TAG, "F0 96");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0x36, &cmd_36, 1), TAG, "MADCTL");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0x3A, &cmd_3a, 1), TAG, "COLMOD");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0xE8, cmd_e8, sizeof(cmd_e8)), TAG, "E8");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0xC5, &cmd_c5, 1), TAG, "C5");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0xC2, &cmd_c2, 1), TAG, "C2");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0xE0, cmd_e0, sizeof(cmd_e0)), TAG, "E0");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0xE1, cmd_e1, sizeof(cmd_e1)), TAG, "E1");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0xF0, &cmd_f0_3c, 1), TAG, "F0 3C");
    ESP_RETURN_ON_ERROR(lcd_write_reg(0xF0, &cmd_f0_69, 1), TAG, "F0 69");

    ESP_RETURN_ON_ERROR(lcd_write_cmd(0x29), TAG, "DISPON");     /* Display On */
    vTaskDelay(pdMS_TO_TICKS(120));

    return ESP_OK;
}

/* ── Static text rendering ──────────────────────────────────────────── */

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

/* Font scaling factor — each 8×16 font pixel becomes SCALE×SCALE screen pixels */
#define FONT_SCALE 2

static void draw_char(uint16_t *fb, int x, int y, char c, uint16_t color)
{
    int idx = (c - FONT_8X16_FIRST_CHAR) * FONT_8X16_BYTES_PER_CHAR;
    if (idx < 0 || idx >= 95 * 16) return;

    for (int row = 0; row < FONT_8X16_CHAR_HEIGHT; row++) {
        uint8_t bits = font_8x16[idx + row];
        for (int col = 0; col < FONT_8X16_CHAR_WIDTH; col++) {
            if (!(bits & (0x80 >> col))) continue;
            /* Draw SCALE×SCALE block for each font pixel */
            for (int dy = 0; dy < FONT_SCALE; dy++) {
                int py = y + row * FONT_SCALE + dy;
                if (py >= LCD_V_RES) break;
                for (int dx = 0; dx < FONT_SCALE; dx++) {
                    int px = x + col * FONT_SCALE + dx;
                    if (px >= LCD_H_RES) break;
                    fb[py * LCD_H_RES + px] = color;
                }
            }
        }
    }
}

static void draw_text(uint16_t *fb, int x, int y, const char *text, uint16_t color)
{
    while (*text) {
        if (*text >= FONT_8X16_FIRST_CHAR && *text <= FONT_8X16_LAST_CHAR) {
            draw_char(fb, x, y, *text, color);
        }
        x += FONT_8X16_CHAR_WIDTH * FONT_SCALE;
        text++;
    }
}

#define BAND_ROWS 32

static esp_err_t display_static_text(void)
{
    /* Full framebuffer is 320*480*2 = 307200 bytes — allocate from PSRAM */
    size_t fb_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    uint16_t *fb = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fb) {
        ESP_LOGE(TAG, "failed to allocate framebuffer");
        return ESP_ERR_NO_MEM;
    }

    /* Fill with dark blue background */
    uint16_t bg = rgb565(0, 0, 40);
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        fb[i] = bg;
    }

    /* Draw two centered lines of text */
    const char *line1 = "USB Mass Storage";
    const char *line2 = "  Demo Running   ";
    int cw = FONT_8X16_CHAR_WIDTH * FONT_SCALE;
    int ch = FONT_8X16_CHAR_HEIGHT * FONT_SCALE;
    int len1 = strlen(line1);
    int len2 = strlen(line2);
    int x1 = (LCD_H_RES - len1 * cw) / 2;
    int x2 = (LCD_H_RES - len2 * cw) / 2;
    int y = (LCD_V_RES - 2 * ch) / 2;

    uint16_t fg = rgb565(255, 255, 255);
    draw_text(fb, x1, y, line1, fg);
    draw_text(fb, x2, y + ch, line2, fg);

    /* Send framebuffer to LCD: set window then write pixel data */
    /* CASET: column address set */
    {
        uint8_t param[4] = {0, 0, (LCD_H_RES - 1) >> 8, (LCD_H_RES - 1) & 0xFF};
        ESP_RETURN_ON_ERROR(lcd_write_reg(0x2A, param, 4), TAG, "CASET");
    }
    /* RASET: row address set */
    {
        uint8_t param[4] = {0, 0, (LCD_V_RES - 1) >> 8, (LCD_V_RES - 1) & 0xFF};
        ESP_RETURN_ON_ERROR(lcd_write_reg(0x2B, param, 4), TAG, "RASET");
    }

    /* RAMWR — send framebuffer in bands to avoid huge SPI transactions */
    ESP_RETURN_ON_ERROR(lcd_write_cmd(0x2C), TAG, "RAMWR");

    for (int yb = 0; yb < LCD_V_RES; yb += BAND_ROWS) {
        int rows = (yb + BAND_ROWS <= LCD_V_RES) ? BAND_ROWS : (LCD_V_RES - yb);
        size_t tx_size = LCD_H_RES * rows * sizeof(uint16_t);
        ESP_RETURN_ON_ERROR(lcd_write_data(fb + yb * LCD_H_RES, tx_size), TAG, "pixel data");
    }

    free(fb);
    ESP_LOGI(TAG, "static text displayed");
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t lcd_init_and_show_text(void)
{
    /* Step 1: I2C + XL9555 for LCD power/reset/backlight */
    ESP_RETURN_ON_ERROR(i2c_xl9555_init(), TAG, "i2c/xl9555 init");

    ESP_RETURN_ON_ERROR(xl9555_output_init(1, XL9555_TFT_RES_PIN, 1), TAG, "tft res out");
    ESP_RETURN_ON_ERROR(xl9555_output_init(1, XL9555_TFT_BLK_PIN, 0), TAG, "tft blk out");

    /* Step 2: SPI2 bus init */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SPI2_CLK,
        .mosi_io_num = PIN_SPI2_MOSI,
        .miso_io_num = PIN_SPI2_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * BAND_ROWS * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi2 init");

    /* Step 3: Add LCD device on SPI2 */
    spi_device_interface_config_t lcd_cfg = {
        .mode = 0,
        .clock_speed_hz = 40 * 1000 * 1000,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 2,
        .pre_cb = lcd_spi_pre_transfer_cb,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &lcd_cfg, &s_lcd_spi), TAG, "lcd spi add");

    /* Step 4: Init LCD DC pin as output */
    gpio_config_t dc_cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_DC,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&dc_cfg);
    gpio_set_level(PIN_LCD_DC, 0);

    /* Step 5: Toggle LCD reset via XL9555 */
    ESP_RETURN_ON_ERROR(xl9555_set_pin_level(1, XL9555_TFT_RES_PIN, 0), TAG, "reset low");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(xl9555_set_pin_level(1, XL9555_TFT_RES_PIN, 1), TAG, "reset high");
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Step 6: Panel init sequence */
    ESP_RETURN_ON_ERROR(lcd_panel_init(), TAG, "panel init");

    /* Step 7: Display static text */
    ESP_RETURN_ON_ERROR(display_static_text(), TAG, "display text");

    /*
     * De-assert LCD CS permanently and reclaim pin as GPIO output high.
     * LCD and SD card share SPI2; without this, SD card traffic on the bus
     * is interpreted by the LCD controller as display data, corrupting the
     * framebuffer and causing visible flickering.
     */
    gpio_config_t cs_hold = {
        .pin_bit_mask = 1ULL << PIN_LCD_CS,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cs_hold);
    gpio_set_level(PIN_LCD_CS, 1);

    /* Step 8: Backlight on */
    ESP_RETURN_ON_ERROR(xl9555_set_pin_level(1, XL9555_TFT_BLK_PIN, 1), TAG, "backlight on");

    ESP_LOGI(TAG, "LCD init complete — SPI2 bus now free for SD card");
    return ESP_OK;
}

int lcd_get_spi_host(void)
{
    return SPI2_HOST;
}
