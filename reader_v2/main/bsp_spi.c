#include "bsp_spi.h"
#include "freertos/semphr.h"

static bool s_spi2_lcd_initialized = false;
static SemaphoreHandle_t s_spi2_bus_lock = NULL;
static StaticSemaphore_t s_spi2_bus_lock_buffer;

static esp_err_t bsp_spi2_bus_lock_init(void)
{
    if (s_spi2_bus_lock != NULL) {
        return ESP_OK;
    }

    s_spi2_bus_lock = xSemaphoreCreateBinaryStatic(&s_spi2_bus_lock_buffer);
    if (s_spi2_bus_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_spi2_bus_lock);
    return ESP_OK;
}

esp_err_t bsp_spi2_lcd_init(void)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BSP_LCD_SPI_CLK_IO,
        .mosi_io_num = BSP_LCD_SPI_MOSI_IO,
        .miso_io_num = BSP_LCD_SPI_MISO_IO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_SPI_MAX_TRANSFER_SZ,
    };
    esp_err_t ret;

    if (s_spi2_lcd_initialized) {
        return ESP_OK;
    }

    ret = bsp_spi2_bus_lock_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if ((ret == ESP_OK) || (ret == ESP_ERR_INVALID_STATE)) {
        s_spi2_lcd_initialized = true;
        return ESP_OK;
    }

    return ret;
}

bool bsp_spi2_lcd_is_initialized(void)
{
    return s_spi2_lcd_initialized;
}

spi_host_device_t bsp_get_spi2_lcd_host(void)
{
    return BSP_LCD_SPI_HOST;
}

esp_err_t bsp_spi2_bus_lock(TickType_t timeout)
{
    if (s_spi2_bus_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return (xSemaphoreTake(s_spi2_bus_lock, timeout) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void bsp_spi2_bus_unlock(void)
{
    if (s_spi2_bus_lock != NULL) {
        xSemaphoreGive(s_spi2_bus_lock);
    }
}

bool bsp_spi2_bus_unlock_from_isr(void)
{
    BaseType_t high_task_wakeup = pdFALSE;

    if (s_spi2_bus_lock != NULL) {
        xSemaphoreGiveFromISR(s_spi2_bus_lock, &high_task_wakeup);
    }

    return high_task_wakeup == pdTRUE;
}
