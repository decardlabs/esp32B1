/*
 * QR-Code-generator library (Project Nayuki, MIT License)
 * Single-function integration wrapper for LVGL image rendering.
 */
#include "qrcode_utils.h"
#include "qrcodegen.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "QRCODE";

/* Helper: allocate and render QR image at given scale */
static lv_img_dsc_t *qrcode_render_at_scale(uint8_t *qrcode, int size, uint8_t scale)
{
    int img_size = size * scale;
    int data_size = img_size * img_size * sizeof(lv_color_t);

    ESP_LOGI(TAG, "trying scale=%d, img=%dx%d, data=%d bytes", scale, img_size, img_size, data_size);

    /* Try internal memory first, fall back to any 8-bit capable memory */
    lv_color_t *data = (lv_color_t *)heap_caps_malloc(data_size, MALLOC_CAP_8BIT);
    if (!data) {
        ESP_LOGW(TAG, "heap_caps_malloc(%d) failed at scale %d", data_size, scale);
        return NULL;
    }

    for (int y = 0; y < img_size; y++) {
        for (int x = 0; x < img_size; x++) {
            int qy = y / scale;
            int qx = x / scale;
            bool dark = qrcodegen_getModule(qrcode, qx, qy);
            data[y * img_size + x] = dark ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
        }
    }

    lv_img_dsc_t *dsc = (lv_img_dsc_t *)malloc(sizeof(lv_img_dsc_t));
    if (!dsc) {
        free(data);
        ESP_LOGE(TAG, "malloc img desc failed");
        return NULL;
    }

    dsc->header.always_zero = 0;
    dsc->header.w = img_size;
    dsc->header.h = img_size;
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
    dsc->data_size = data_size;
    dsc->data = (const uint8_t *)data;

    return dsc;
}

lv_img_dsc_t *qrcode_get_lvgl_image(const char *text, uint8_t scale)
{
    if (!text || strlen(text) == 0 || scale == 0) {
        ESP_LOGE(TAG, "Invalid input");
        return NULL;
    }

    /* Encode QR code */
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(text, temp, qrcode,
                                   qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN,
                                   qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) {
        ESP_LOGE(TAG, "QR encode failed for text: %s", text);
        return NULL;
    }

    int size = qrcodegen_getSize(qrcode);
    ESP_LOGI(TAG, "QR size=%d modules", size);

    /* Try requested scale, then fall back to smaller scales */
    uint8_t scales[] = {scale, (uint8_t)(scale / 2), (uint8_t)(scale / 4), 1};
    for (int i = 0; i < 4; i++) {
        if (scales[i] < 1) continue;
        lv_img_dsc_t *dsc = qrcode_render_at_scale(qrcode, size, scales[i]);
        if (dsc) {
            return dsc;
        }
        /* Before trying smaller, wait briefly for memory pressure to ease */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGE(TAG, "all allocation attempts failed");
    return NULL;
}
