#include "xpt2046.h"

#include "sdkconfig.h"
#include "bsp_spi.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define XPT2046_CMD_Y    0x90
#define XPT2046_CMD_X    0xD0
#define XPT2046_CMD_Z1   0xB0

#define XPT2046_SPI_HZ               (500 * 1000)
#define XPT2046_TOUCH_CS_GPIO        BSP_GPIO_TOUCH_CS
#define XPT2046_TOUCH_Z1_THRESHOLD   20
#define XPT2046_TOUCH_Z1_RELEASE     8
#define XPT2046_SAMPLE_COUNT         4

// Calibrated range for common 320x480 ST7796 + XPT2046 modules.
#define XPT2046_RAW_X_MIN            220
#define XPT2046_RAW_X_MAX            3850
#define XPT2046_RAW_Y_MIN            260
#define XPT2046_RAW_Y_MAX            3820

#define XPT2046_SWAP_XY              0
#define XPT2046_MIRROR_X             0
#define XPT2046_MIRROR_Y             0

static const char *TAG = "XPT2046";

static spi_device_handle_t s_touch_dev = NULL;
static int s_spi_mode = 0;
static bool s_touch_irq_ready = false;
static uint16_t s_last_z1 = 0;
static uint16_t s_last_raw_x = 0;
static uint16_t s_last_raw_y = 0;
static bool s_last_touched = false;

static inline bool xpt2046_irq_is_pressed(void)
{
    if (!s_touch_irq_ready) {
        return false;
    }
    // XPT2046 IRQ is active-low while panel is touched.
    return gpio_get_level(BSP_GPIO_TOUCH_IRQ) == 0;
}

static esp_err_t xpt2046_add_device_with_mode(int mode, spi_device_handle_t *out_dev)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = XPT2046_SPI_HZ,
        .mode = mode,
        .spics_io_num = XPT2046_TOUCH_CS_GPIO,
        .queue_size = 1,
        .flags = 0,
    };

    ESP_RETURN_ON_FALSE(out_dev != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    return spi_bus_add_device(bsp_get_spi2_lcd_host(), &devcfg, out_dev);
}

static inline uint16_t xpt2046_clamp_u16(uint16_t v, uint16_t min_v, uint16_t max_v)
{
    if (v < min_v) {
        return min_v;
    }
    if (v > max_v) {
        return max_v;
    }
    return v;
}

static esp_err_t xpt2046_read_channel(uint8_t cmd, uint16_t *value)
{
    uint8_t tx_buf[3] = {cmd, 0x00, 0x00};
    uint8_t rx_buf[3] = {0};
    spi_transaction_t t = {
        .length = 24,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };

    ESP_RETURN_ON_FALSE((s_touch_dev != NULL) && (value != NULL), ESP_ERR_INVALID_ARG, TAG, "touch not ready");
    ESP_RETURN_ON_ERROR(spi_device_polling_transmit(s_touch_dev, &t), TAG, "spi transmit failed");

    *value = (uint16_t)(((rx_buf[1] << 8) | rx_buf[2]) >> 3) & 0x0FFF;
    return ESP_OK;
}

static uint16_t xpt2046_map_axis(uint16_t raw, uint16_t raw_min, uint16_t raw_max, uint16_t screen_max)
{
    uint32_t num;

    if (raw_max <= raw_min || screen_max == 0) {
        return 0;
    }

    raw = xpt2046_clamp_u16(raw, raw_min, raw_max);
    num = (uint32_t)(raw - raw_min) * (uint32_t)(screen_max - 1);
    return (uint16_t)(num / (uint32_t)(raw_max - raw_min));
}

esp_err_t xpt2046_init(void)
{
    // This board has verified stable touch behavior in mode 0.
    const int fixed_mode = 0;

    if (s_touch_dev != NULL) {
        return ESP_OK;
    }

    {
        gpio_config_t irq_cfg = {
            .pin_bit_mask = (1ULL << BSP_GPIO_TOUCH_IRQ),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&irq_cfg) == ESP_OK) {
            s_touch_irq_ready = true;
        }
    }

    ESP_RETURN_ON_FALSE(bsp_spi2_lcd_is_initialized(), ESP_ERR_INVALID_STATE, TAG, "call bsp_spi2_lcd_init first");

    ESP_RETURN_ON_ERROR(xpt2046_add_device_with_mode(fixed_mode, &s_touch_dev), TAG, "add spi device failed");
    s_spi_mode = fixed_mode;

    ESP_LOGI(TAG, "xpt2046 init done, fixed mode=%d", s_spi_mode);
    return ESP_OK;
}

bool xpt2046_is_initialized(void)
{
    return s_touch_dev != NULL;
}

esp_err_t xpt2046_read_point(uint16_t screen_w,
                             uint16_t screen_h,
                             uint16_t *x,
                             uint16_t *y,
                             bool *pressed)
{
    uint16_t z1 = 0;
    uint32_t x_sum = 0;
    uint32_t y_sum = 0;
    uint16_t x_raw;
    uint16_t y_raw;
    uint16_t x_mapped;
    uint16_t y_mapped;
    bool raw_valid = false;
    bool touched = false;
    bool irq_pressed = false;

    ESP_RETURN_ON_FALSE((x != NULL) && (y != NULL) && (pressed != NULL), ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    ESP_RETURN_ON_FALSE(s_touch_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "touch not initialized");

    ESP_RETURN_ON_ERROR(bsp_spi2_bus_lock(pdMS_TO_TICKS(20)), TAG, "spi bus lock timeout");

    // Read pressure channel first, then still sample X/Y to support panels where Z1 is weak.
    if (xpt2046_read_channel(XPT2046_CMD_Z1, &z1) != ESP_OK) {
        bsp_spi2_bus_unlock();
        return ESP_FAIL;
    }

    for (int i = 0; i < XPT2046_SAMPLE_COUNT; i++) {
        if (xpt2046_read_channel(XPT2046_CMD_X, &x_raw) != ESP_OK ||
            xpt2046_read_channel(XPT2046_CMD_Y, &y_raw) != ESP_OK) {
            bsp_spi2_bus_unlock();
            return ESP_FAIL;
        }
        x_sum += x_raw;
        y_sum += y_raw;
    }

    bsp_spi2_bus_unlock();

    x_raw = (uint16_t)(x_sum / XPT2046_SAMPLE_COUNT);
    y_raw = (uint16_t)(y_sum / XPT2046_SAMPLE_COUNT);
    s_last_z1 = z1;
    s_last_raw_x = x_raw;
    s_last_raw_y = y_raw;

    raw_valid = true;

    irq_pressed = xpt2046_irq_is_pressed();

    if (s_touch_irq_ready) {
        touched = irq_pressed;
    } else {
        // Fallback path when IRQ is unavailable.
        if (s_last_touched) {
            touched = (z1 > XPT2046_TOUCH_Z1_RELEASE);
        } else {
            touched = (z1 >= XPT2046_TOUCH_Z1_THRESHOLD);
        }
    }

    // Guard against obvious noise while keeping valid touches responsive.
    if (touched && !raw_valid && !s_touch_irq_ready) {
        touched = false;
    }

    s_last_touched = touched;
    if (!touched) {
        *pressed = false;
        return ESP_OK;
    }

#if XPT2046_SWAP_XY
    {
        uint16_t tmp = x_raw;
        x_raw = y_raw;
        y_raw = tmp;
    }
#endif

    x_mapped = xpt2046_map_axis(x_raw, XPT2046_RAW_X_MIN, XPT2046_RAW_X_MAX, screen_w);
    y_mapped = xpt2046_map_axis(y_raw, XPT2046_RAW_Y_MIN, XPT2046_RAW_Y_MAX, screen_h);

#if XPT2046_MIRROR_X
    x_mapped = (screen_w > 0) ? (uint16_t)((screen_w - 1) - x_mapped) : 0;
#endif
#if XPT2046_MIRROR_Y
    y_mapped = (screen_h > 0) ? (uint16_t)((screen_h - 1) - y_mapped) : 0;
#endif

    *x = x_mapped;
    *y = y_mapped;
    *pressed = true;

    return ESP_OK;
}

esp_err_t xpt2046_get_last_sample(uint16_t *z1,
                                  uint16_t *raw_x,
                                  uint16_t *raw_y,
                                  bool *touched)
{
    ESP_RETURN_ON_FALSE((z1 != NULL) && (raw_x != NULL) && (raw_y != NULL) && (touched != NULL),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid arg");

    *z1 = s_last_z1;
    *raw_x = s_last_raw_x;
    *raw_y = s_last_raw_y;
    *touched = s_last_touched;
    return ESP_OK;
}

esp_err_t xpt2046_get_debug_info(int *spi_mode)
{
    ESP_RETURN_ON_FALSE(spi_mode != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    *spi_mode = s_spi_mode;
    return ESP_OK;
}

esp_err_t xpt2046_set_spi_mode(int spi_mode)
{
    spi_device_handle_t old_dev;
    int old_mode;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE((spi_mode >= 0) && (spi_mode <= 3), ESP_ERR_INVALID_ARG, TAG, "invalid mode");
    ESP_RETURN_ON_FALSE(s_touch_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "touch not initialized");

    if (spi_mode == s_spi_mode) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bsp_spi2_bus_lock(pdMS_TO_TICKS(50)), TAG, "spi bus lock timeout");

    old_dev = s_touch_dev;
    old_mode = s_spi_mode;

    s_touch_dev = NULL;
    spi_bus_remove_device(old_dev);

    ret = xpt2046_add_device_with_mode(spi_mode, &s_touch_dev);
    if (ret == ESP_OK) {
        s_spi_mode = spi_mode;
        bsp_spi2_bus_unlock();
        ESP_LOGI(TAG, "touch spi mode switched to %d", s_spi_mode);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "switch mode %d failed, rollback to %d", spi_mode, old_mode);
    s_touch_dev = NULL;
    if (xpt2046_add_device_with_mode(old_mode, &s_touch_dev) == ESP_OK) {
        s_spi_mode = old_mode;
    }
    bsp_spi2_bus_unlock();
    return ret;
}
