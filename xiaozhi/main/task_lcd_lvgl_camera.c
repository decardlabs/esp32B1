#include <string.h>

#include "face_recognition_service.h"
#include "task_lcd_lvgl_camera.h"
#include "lcd_st7796.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "xl9555.h"

#define LCD_LVGL_CAMERA_BUF_LINES  24
#define LCD_CAMERA_KEY_LONG_MS     1500
#define LCD_CAMERA_BOX_BORDER      3
#define LCD_CAMERA_BEEP_MS         110
#define LCD_CAMERA_FRAME_WIDTH     320
#define LCD_CAMERA_FRAME_HEIGHT    240
#define LCD_CAMERA_SRC_WIDTH       320
#define LCD_CAMERA_SRC_HEIGHT      240
#define LCD_CAMERA_FB_COUNT        1
#define OV2640_XCLK_HZ             20000000

#define OV2640_PIN_SIOD            GPIO_NUM_39
#define OV2640_PIN_SIOC            GPIO_NUM_38
#define OV2640_PIN_D7              GPIO_NUM_18
#define OV2640_PIN_D6              GPIO_NUM_17
#define OV2640_PIN_D5              GPIO_NUM_16
#define OV2640_PIN_D4              GPIO_NUM_15
#define OV2640_PIN_D3              GPIO_NUM_7
#define OV2640_PIN_D2              GPIO_NUM_6
#define OV2640_PIN_D1              GPIO_NUM_5
#define OV2640_PIN_D0              GPIO_NUM_4
#define OV2640_PIN_VSYNC           GPIO_NUM_47
#define OV2640_PIN_HREF            GPIO_NUM_48
#define OV2640_PIN_PCLK            GPIO_NUM_45

typedef struct {
    bool key1_raw_pressed;
    bool key1_stable_pressed;
    bool key1_long_handled;
    TickType_t key1_last_change_tick;
    TickType_t key1_press_tick;
    bool camera_ready;
    bool camera_has_good_frame;
    uint32_t camera_bad_frame_count;
    uint32_t last_face_alert_seq;
    bool beep_active;
    TickType_t beep_release_tick;
    bool overlay_pending;
    lv_color_t *camera_buf;
    uint16_t *frame_buf;
    uint16_t *good_frame_buf;
    lcd_lvgl_camera_bindings_t bindings;
} lcd_lvgl_camera_ctx_t;

static const char *TAG = "LCD_CAMERA";
static lcd_lvgl_camera_ctx_t g_lcd_camera = {0};

static void lcd_lvgl_camera_set_beep_idle(void)
{
    xl9555_set_pin_level(BEEP_PORT, BEEP_PIN, 1);
}

static void lcd_lvgl_camera_face_alert_beep_start(void)
{
    xl9555_set_pin_level(BEEP_PORT, BEEP_PIN, 0);
    g_lcd_camera.beep_active = true;
    g_lcd_camera.beep_release_tick = xTaskGetTickCount() + pdMS_TO_TICKS(LCD_CAMERA_BEEP_MS);
}

static void lcd_lvgl_camera_handle_face_alert(const face_recognition_snapshot_t *snapshot)
{
    if ((snapshot == NULL) ||
        (snapshot->alert_seq == 0) ||
        (snapshot->alert_seq == g_lcd_camera.last_face_alert_seq) ||
        (snapshot->alert_text[0] == '\0')) {
        return;
    }

    g_lcd_camera.last_face_alert_seq = snapshot->alert_seq;
    lcd_lvgl_camera_face_alert_beep_start();
    ESP_LOGI(TAG, "face alert: %s", snapshot->alert_text);
}

static void lcd_lvgl_camera_fill_black(void)
{
    size_t buf_size;
    int lines_per_flush;

    if ((g_lcd_camera.bindings.fallback_buf == NULL) ||
        (g_lcd_camera.bindings.io_done_sem == NULL) ||
        (g_lcd_camera.bindings.fallback_buf_lines <= 0)) {
        return;
    }

    lines_per_flush = g_lcd_camera.bindings.fallback_buf_lines;
    buf_size = LCD_ST7796_H_RES * lines_per_flush * sizeof(lv_color_t);
    memset(g_lcd_camera.bindings.fallback_buf, 0, buf_size);

    for (int y = 0; y < LCD_ST7796_V_RES; y += lines_per_flush) {
        int lines = ((y + lines_per_flush) <= LCD_ST7796_V_RES) ?
                    lines_per_flush :
                    (LCD_ST7796_V_RES - y);

        xSemaphoreTake(g_lcd_camera.bindings.io_done_sem, 0);
        if (lcd_st7796_draw_bitmap_owned(0,
                                         y,
                                         LCD_ST7796_H_RES,
                                         y + lines,
                                         g_lcd_camera.bindings.fallback_buf,
                                         LCD_ST7796_TRANSFER_DIRECT) != ESP_OK) {
            ESP_LOGW(TAG, "fill black flush failed at y=%d", y);
            break;
        }
        xSemaphoreTake(g_lcd_camera.bindings.io_done_sem, portMAX_DELAY);
    }
}

static lv_color_t *lcd_lvgl_camera_get_draw_buf(void)
{
    if (g_lcd_camera.camera_buf != NULL) {
        return g_lcd_camera.camera_buf;
    }

    return g_lcd_camera.bindings.fallback_buf;
}

static int lcd_lvgl_camera_get_draw_buf_lines(void)
{
    if (g_lcd_camera.camera_buf != NULL) {
        return LCD_LVGL_CAMERA_BUF_LINES;
    }

    if (g_lcd_camera.bindings.fallback_buf_lines > 0) {
        return g_lcd_camera.bindings.fallback_buf_lines;
    }

    return 1;
}

static uint16_t *lcd_lvgl_camera_get_frame_buf(void)
{
    if (g_lcd_camera.frame_buf == NULL) {
        g_lcd_camera.frame_buf = heap_caps_malloc(LCD_CAMERA_FRAME_WIDTH * LCD_CAMERA_FRAME_HEIGHT * sizeof(uint16_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_lcd_camera.frame_buf == NULL) {
            g_lcd_camera.frame_buf = heap_caps_malloc(LCD_CAMERA_FRAME_WIDTH * LCD_CAMERA_FRAME_HEIGHT * sizeof(uint16_t),
                                                      MALLOC_CAP_8BIT);
        }
        if (g_lcd_camera.frame_buf == NULL) {
            ESP_LOGE(TAG, "camera frame buffer alloc failed");
        }
    }

    return g_lcd_camera.frame_buf;
}

static uint16_t *lcd_lvgl_camera_get_good_frame_buf(void)
{
    if (g_lcd_camera.good_frame_buf == NULL) {
        g_lcd_camera.good_frame_buf = heap_caps_malloc(LCD_CAMERA_FRAME_WIDTH * LCD_CAMERA_FRAME_HEIGHT * sizeof(uint16_t),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_lcd_camera.good_frame_buf == NULL) {
            g_lcd_camera.good_frame_buf = heap_caps_malloc(LCD_CAMERA_FRAME_WIDTH * LCD_CAMERA_FRAME_HEIGHT * sizeof(uint16_t),
                                                           MALLOC_CAP_8BIT);
        }
        if (g_lcd_camera.good_frame_buf == NULL) {
            ESP_LOGE(TAG, "camera good frame buffer alloc failed");
        }
    }

    return g_lcd_camera.good_frame_buf;
}

static inline int lcd_lvgl_camera_rgb565_delta(uint16_t a, uint16_t b)
{
    int dr = ((a >> 11) & 0x1F) - ((b >> 11) & 0x1F);
    int dg = ((a >> 5) & 0x3F) - ((b >> 5) & 0x3F);
    int db = (a & 0x1F) - (b & 0x1F);

    if (dr < 0) {
        dr = -dr;
    }
    if (dg < 0) {
        dg = -dg;
    }
    if (db < 0) {
        db = -db;
    }

    return (dr * 8) + (dg * 4) + (db * 8);
}

static bool lcd_lvgl_camera_frame_is_corrupted(const uint16_t *current,
                                               const uint16_t *reference,
                                               uint16_t width,
                                               uint16_t height)
{
    int upper_samples = 0;
    int lower_samples = 0;
    int upper_changed = 0;
    int lower_changed = 0;
    int upper_noise = 0;
    int lower_noise = 0;
    const int x_step = 8;
    const int y_step = 6;

    if ((current == NULL) || (reference == NULL) || (width < 32) || (height < 32)) {
        return false;
    }

    for (uint16_t y = 0; (y + y_step) < height; y += y_step) {
        bool lower_half = (y >= (height / 2));

        for (uint16_t x = 0; (x + x_step) < width; x += x_step) {
            size_t index = ((size_t)y * width) + x;
            int delta_prev = lcd_lvgl_camera_rgb565_delta(current[index], reference[index]);
            int delta_x = lcd_lvgl_camera_rgb565_delta(current[index], current[index + x_step]);
            int delta_y = lcd_lvgl_camera_rgb565_delta(current[index], current[index + ((size_t)y_step * width)]);

            if (lower_half) {
                lower_samples++;
                if (delta_prev > 150) {
                    lower_changed++;
                }
                if ((delta_x > 180) || (delta_y > 180)) {
                    lower_noise++;
                }
            } else {
                upper_samples++;
                if (delta_prev > 150) {
                    upper_changed++;
                }
                if ((delta_x > 180) || (delta_y > 180)) {
                    upper_noise++;
                }
            }
        }
    }

    if ((upper_samples == 0) || (lower_samples == 0)) {
        return false;
    }

    {
        float upper_change_ratio = (float)upper_changed / (float)upper_samples;
        float lower_change_ratio = (float)lower_changed / (float)lower_samples;
        float upper_noise_ratio = (float)upper_noise / (float)upper_samples;
        float lower_noise_ratio = (float)lower_noise / (float)lower_samples;

        return ((upper_change_ratio < 0.42f) &&
                (lower_change_ratio > 0.82f) &&
                ((lower_change_ratio - upper_change_ratio) > 0.34f)) ||
               ((upper_noise_ratio < 0.40f) &&
                (lower_noise_ratio > 0.72f) &&
                ((lower_noise_ratio - upper_noise_ratio) > 0.28f));
    }
}

static bool lcd_lvgl_camera_key1_is_pressed(void)
{
    bool key_level = true;

    xl9555_get_pin_level(KEY_PORT, KEY1_PIN, &key_level);
    return (key_level == 0);
}

static void lcd_lvgl_camera_key_reset(void)
{
    g_lcd_camera.key1_raw_pressed = false;
    g_lcd_camera.key1_stable_pressed = false;
    g_lcd_camera.key1_long_handled = false;
    g_lcd_camera.key1_last_change_tick = xTaskGetTickCount();
    g_lcd_camera.key1_press_tick = 0;
}

static void lcd_lvgl_camera_power_init(void)
{
    xl9555_set_pin_level(OV_PORT, OV_RESET_PIN, 0);
    xl9555_set_pin_level(OV_PORT, OV_PWDN_PIN, 1);
}

static void lcd_lvgl_camera_power_cycle(void)
{
    lcd_lvgl_camera_power_init();
    xl9555_set_pin_level(OV_PORT, OV_PWDN_PIN, 1);
    xl9555_set_pin_level(OV_PORT, OV_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9555_set_pin_level(OV_PORT, OV_PWDN_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    xl9555_set_pin_level(OV_PORT, OV_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void lcd_lvgl_camera_apply_preview_profile(sensor_t *sensor)
{
    if (sensor == NULL) {
        return;
    }

    sensor->set_pixformat(sensor, PIXFORMAT_RGB565);
    sensor->set_framesize(sensor, FRAMESIZE_QVGA);
    sensor->set_hmirror(sensor, 1);
    sensor->set_vflip(sensor, 1);

    if (sensor->set_brightness != NULL) {
        sensor->set_brightness(sensor, 0);
    }
    if (sensor->set_contrast != NULL) {
        sensor->set_contrast(sensor, 1);
    }
    if (sensor->set_saturation != NULL) {
        sensor->set_saturation(sensor, 0);
    }
    if (sensor->set_sharpness != NULL) {
        sensor->set_sharpness(sensor, 2);
    }
    if (sensor->set_denoise != NULL) {
        sensor->set_denoise(sensor, 0);
    }
    if (sensor->set_whitebal != NULL) {
        sensor->set_whitebal(sensor, 1);
    }
    if (sensor->set_gain_ctrl != NULL) {
        sensor->set_gain_ctrl(sensor, 1);
    }
    if (sensor->set_exposure_ctrl != NULL) {
        sensor->set_exposure_ctrl(sensor, 1);
    }
}

static esp_err_t lcd_lvgl_camera_init(void)
{
    camera_config_t camera_config = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = -1,
        .pin_sccb_sda = OV2640_PIN_SIOD,
        .pin_sccb_scl = OV2640_PIN_SIOC,
        .pin_d7 = OV2640_PIN_D7,
        .pin_d6 = OV2640_PIN_D6,
        .pin_d5 = OV2640_PIN_D5,
        .pin_d4 = OV2640_PIN_D4,
        .pin_d3 = OV2640_PIN_D3,
        .pin_d2 = OV2640_PIN_D2,
        .pin_d1 = OV2640_PIN_D1,
        .pin_d0 = OV2640_PIN_D0,
        .pin_vsync = OV2640_PIN_VSYNC,
        .pin_href = OV2640_PIN_HREF,
        .pin_pclk = OV2640_PIN_PCLK,
        .xclk_freq_hz = OV2640_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = LCD_CAMERA_FB_COUNT,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location = CAMERA_FB_IN_PSRAM,
    };
    sensor_t *sensor;
    esp_err_t ret;

    if (g_lcd_camera.camera_ready) {
        return ESP_OK;
    }

    lcd_lvgl_camera_power_cycle();
    ret = esp_camera_init(&camera_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_camera_set_psram_mode(true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "enable camera psram dma failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "camera psram dma enabled");
    }

    sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        lcd_lvgl_camera_apply_preview_profile(sensor);
    }

    g_lcd_camera.camera_ready = true;
    g_lcd_camera.overlay_pending = false;
    g_lcd_camera.camera_has_good_frame = false;
    g_lcd_camera.camera_bad_frame_count = 0;
    ESP_LOGI(TAG, "camera preview ready, rgb565=%ux%u", LCD_CAMERA_SRC_WIDTH, LCD_CAMERA_SRC_HEIGHT);
    return ESP_OK;
}

static void lcd_lvgl_camera_deinit(void)
{
    if (!g_lcd_camera.camera_ready) {
        return;
    }

    esp_camera_deinit();
    xl9555_set_pin_level(OV_PORT, OV_RESET_PIN, 0);
    xl9555_set_pin_level(OV_PORT, OV_PWDN_PIN, 1);
    g_lcd_camera.camera_ready = false;
    g_lcd_camera.overlay_pending = false;
}

static void lcd_lvgl_camera_update_status(const face_recognition_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    if (g_lcd_camera.bindings.note_label != NULL) {
        lv_label_set_text(g_lcd_camera.bindings.note_label,
                          (snapshot->status[0] != '\0') ? snapshot->status : "人脸识别运行中");
    }
    g_lcd_camera.overlay_pending = true;
}

static void lcd_lvgl_camera_draw_box_edge(uint16_t *pixels,
                                          int width,
                                          int height,
                                          int left,
                                          int top,
                                          int right,
                                          int bottom,
                                          uint16_t color)
{
    if ((pixels == NULL) || (width <= 0) || (height <= 0)) {
        return;
    }

    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (right >= width) {
        right = width - 1;
    }
    if (bottom >= height) {
        bottom = height - 1;
    }
    if ((left >= right) || (top >= bottom)) {
        return;
    }

    for (int thickness = 0; thickness < LCD_CAMERA_BOX_BORDER; thickness++) {
        int y1 = top + thickness;
        int y2 = bottom - thickness;
        int x1 = left + thickness;
        int x2 = right - thickness;

        if (y1 <= y2) {
            for (int x = x1; x <= x2; x++) {
                pixels[(y1 * width) + x] = color;
                pixels[(y2 * width) + x] = color;
            }
        }

        if (x1 <= x2) {
            for (int y = y1; y <= y2; y++) {
                pixels[(y * width) + x1] = color;
                pixels[(y * width) + x2] = color;
            }
        }
    }
}

static void lcd_lvgl_camera_draw_face_boxes(uint16_t *pixels,
                                            int width,
                                            int height,
                                            const face_recognition_snapshot_t *snapshot)
{
    if ((pixels == NULL) || (snapshot == NULL)) {
        return;
    }

    for (int i = 0; i < snapshot->box_count; i++) {
        uint16_t color = snapshot->boxes[i].primary ?
                         (snapshot->recognized ? 0x07E0 : 0xFD20) :
                         0x07FF;

        lcd_lvgl_camera_draw_box_edge(pixels,
                                      width,
                                      height,
                                      snapshot->boxes[i].left,
                                      snapshot->boxes[i].top,
                                      snapshot->boxes[i].right,
                                      snapshot->boxes[i].bottom,
                                      color);
    }
}

void lcd_lvgl_camera_init_runtime(void)
{
    lcd_lvgl_camera_set_beep_idle();
    g_lcd_camera.beep_active = false;
    g_lcd_camera.overlay_pending = false;
    g_lcd_camera.last_face_alert_seq = 0;
    lcd_lvgl_camera_key_reset();
}

esp_err_t lcd_lvgl_camera_reserve_buffers(void)
{
    if (g_lcd_camera.camera_buf == NULL) {
        g_lcd_camera.camera_buf = heap_caps_malloc(LCD_CAMERA_FRAME_WIDTH * LCD_LVGL_CAMERA_BUF_LINES * sizeof(lv_color_t),
                                                   MALLOC_CAP_DMA);
        if (g_lcd_camera.camera_buf == NULL) {
            ESP_LOGW(TAG, "camera dma buffer alloc failed, fallback to lvgl buffer");
        }
    }

    (void)lcd_lvgl_camera_get_frame_buf();
    (void)lcd_lvgl_camera_get_good_frame_buf();
    return ESP_OK;
}

void lcd_lvgl_camera_set_bindings(const lcd_lvgl_camera_bindings_t *bindings)
{
    if (bindings == NULL) {
        memset(&g_lcd_camera.bindings, 0, sizeof(g_lcd_camera.bindings));
        return;
    }

    g_lcd_camera.bindings = *bindings;
}

esp_err_t lcd_lvgl_camera_enter(void)
{
    face_recognition_snapshot_t face_snapshot = {0};
    esp_err_t ret;

    if ((g_lcd_camera.bindings.io_done_sem == NULL) || (g_lcd_camera.bindings.fallback_buf == NULL)) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = lcd_lvgl_camera_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = face_recognition_service_init();
    face_recognition_service_get_snapshot(&face_snapshot);
    lcd_lvgl_camera_update_status(&face_snapshot);
    g_lcd_camera.last_face_alert_seq = face_snapshot.alert_seq;
    g_lcd_camera.beep_active = false;
    lcd_lvgl_camera_set_beep_idle();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "face recognition init failed: %s", esp_err_to_name(ret));
    }

    lcd_lvgl_camera_key_reset();
    lcd_lvgl_camera_fill_black();
    g_lcd_camera.overlay_pending = true;
    ESP_LOGI(TAG, "camera preview start");
    return ESP_OK;
}

void lcd_lvgl_camera_leave(void)
{
    lcd_lvgl_camera_key_reset();
    lcd_lvgl_camera_set_beep_idle();
    g_lcd_camera.beep_active = false;
    g_lcd_camera.last_face_alert_seq = 0;
    g_lcd_camera.camera_has_good_frame = false;
    g_lcd_camera.camera_bad_frame_count = 0;
    face_recognition_service_deinit();
    lcd_lvgl_camera_deinit();
    ESP_LOGI(TAG, "camera preview stop");
}

void lcd_lvgl_camera_poll_key(bool camera_view_active)
{
    TickType_t debounce_ticks = pdMS_TO_TICKS(40);
    TickType_t long_press_ticks = pdMS_TO_TICKS(LCD_CAMERA_KEY_LONG_MS);
    TickType_t now;
    bool level_low;

    if (!camera_view_active) {
        lcd_lvgl_camera_key_reset();
        return;
    }

    level_low = lcd_lvgl_camera_key1_is_pressed();
    now = xTaskGetTickCount();

    if (level_low != g_lcd_camera.key1_raw_pressed) {
        g_lcd_camera.key1_raw_pressed = level_low;
        g_lcd_camera.key1_last_change_tick = now;
    }

    if (((now - g_lcd_camera.key1_last_change_tick) >= debounce_ticks) &&
        (g_lcd_camera.key1_stable_pressed != g_lcd_camera.key1_raw_pressed)) {
        g_lcd_camera.key1_stable_pressed = g_lcd_camera.key1_raw_pressed;

        if (g_lcd_camera.key1_stable_pressed) {
            g_lcd_camera.key1_press_tick = now;
            g_lcd_camera.key1_long_handled = false;
        } else if (!g_lcd_camera.key1_long_handled) {
            face_recognition_snapshot_t snapshot;
            bool status_changed = false;

            face_recognition_service_request_enroll(&snapshot, &status_changed);
            if (status_changed) {
                lcd_lvgl_camera_update_status(&snapshot);
            }
        }
    }

    if (g_lcd_camera.key1_stable_pressed &&
        !g_lcd_camera.key1_long_handled &&
        ((now - g_lcd_camera.key1_press_tick) >= long_press_ticks)) {
        face_recognition_snapshot_t snapshot;
        bool status_changed = false;

        face_recognition_service_clear_all(&snapshot, &status_changed);
        g_lcd_camera.key1_long_handled = true;
        if (status_changed) {
            lcd_lvgl_camera_update_status(&snapshot);
        }
    }
}

void lcd_lvgl_camera_service_beep(void)
{
    TickType_t now;

    if (!g_lcd_camera.beep_active) {
        return;
    }

    now = xTaskGetTickCount();
    if ((int32_t)(now - g_lcd_camera.beep_release_tick) >= 0) {
        lcd_lvgl_camera_set_beep_idle();
        g_lcd_camera.beep_active = false;
    }
}

bool lcd_lvgl_camera_take_overlay_pending(void)
{
    bool overlay_pending = g_lcd_camera.overlay_pending;

    g_lcd_camera.overlay_pending = false;
    return overlay_pending;
}

void lcd_lvgl_camera_render_frame(void)
{
    camera_fb_t *fb;
    face_recognition_snapshot_t face_snapshot;
    bool face_status_changed = false;
    lv_color_t *camera_buf;
    uint16_t *frame_buf;
    uint16_t *good_frame_buf;
    const uint8_t *src;
    int camera_buf_lines;
    int x_offset;
    int y_offset;
    int draw_width;
    int draw_height;
    bool scale_2x = false;
    uint16_t frame_width;
    uint16_t frame_height;
    size_t expected_len;
    bool use_good_frame = false;

    if (!g_lcd_camera.camera_ready || (g_lcd_camera.bindings.io_done_sem == NULL)) {
        return;
    }

    fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGW(TAG, "camera frame get failed");
        vTaskDelay(pdMS_TO_TICKS(30));
        return;
    }

    if (fb->format != PIXFORMAT_RGB565) {
        ESP_LOGW(TAG, "unexpected camera format=%d", fb->format);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(30));
        return;
    }

    frame_width = fb->width;
    frame_height = fb->height;
    expected_len = (size_t)frame_width * frame_height * sizeof(uint16_t);
    if (fb->len != expected_len) {
        ESP_LOGW(TAG,
                 "drop abnormal camera frame: len=%u expected=%u size=%ux%u",
                 (unsigned)fb->len,
                 (unsigned)expected_len,
                 (unsigned)frame_width,
                 (unsigned)frame_height);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    frame_buf = lcd_lvgl_camera_get_frame_buf();
    if (frame_buf == NULL) {
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    good_frame_buf = lcd_lvgl_camera_get_good_frame_buf();

    memcpy(frame_buf, fb->buf, expected_len);
    esp_camera_fb_return(fb);

    if ((good_frame_buf != NULL) &&
        g_lcd_camera.camera_has_good_frame &&
        lcd_lvgl_camera_frame_is_corrupted(frame_buf, good_frame_buf, frame_width, frame_height)) {
        use_good_frame = true;
        g_lcd_camera.camera_bad_frame_count++;
        if ((g_lcd_camera.camera_bad_frame_count == 1) ||
            ((g_lcd_camera.camera_bad_frame_count % 8) == 0)) {
            ESP_LOGW(TAG,
                     "drop corrupted camera frame, bad_count=%u, size=%ux%u",
                     (unsigned)g_lcd_camera.camera_bad_frame_count,
                     (unsigned)frame_width,
                     (unsigned)frame_height);
        }
        face_recognition_service_get_snapshot(&face_snapshot);
    } else {
        g_lcd_camera.camera_bad_frame_count = 0;
        if (good_frame_buf != NULL) {
            memcpy(good_frame_buf, frame_buf, expected_len);
            g_lcd_camera.camera_has_good_frame = true;
        }

        if (face_recognition_service_process_frame(frame_buf,
                                                   frame_width,
                                                   frame_height,
                                                   &face_snapshot,
                                                   &face_status_changed)) {
            lcd_lvgl_camera_draw_face_boxes(frame_buf, frame_width, frame_height, &face_snapshot);
            if (face_status_changed) {
                lcd_lvgl_camera_update_status(&face_snapshot);
            }
            lcd_lvgl_camera_handle_face_alert(&face_snapshot);
        }
    }

    if ((frame_width * 2) == LCD_CAMERA_FRAME_WIDTH && (frame_height * 2) == LCD_CAMERA_FRAME_HEIGHT) {
        scale_2x = true;
        draw_width = frame_width * 2;
        draw_height = frame_height * 2;
    } else {
        draw_width = frame_width;
        draw_height = frame_height;
    }

    x_offset = (LCD_ST7796_H_RES - draw_width) / 2;
    y_offset = (LCD_ST7796_V_RES - draw_height) / 2;
    camera_buf = lcd_lvgl_camera_get_draw_buf();
    camera_buf_lines = lcd_lvgl_camera_get_draw_buf_lines();
    if (camera_buf == NULL) {
        return;
    }
    if (x_offset < 0) {
        x_offset = 0;
    }
    if (y_offset < 0) {
        y_offset = 0;
    }

    src = (const uint8_t *)(use_good_frame ? good_frame_buf : frame_buf);
    for (int row = 0; row < frame_height; row += (scale_2x ? (camera_buf_lines / 2) : camera_buf_lines)) {
        int src_lines;
        int out_lines;

        if (scale_2x) {
            const uint16_t *src_pixels = (const uint16_t *)src;
            uint16_t *dst_pixels = (uint16_t *)camera_buf;

            src_lines = camera_buf_lines / 2;
            if (src_lines <= 0) {
                src_lines = 1;
            }
            if ((row + src_lines) > frame_height) {
                src_lines = frame_height - row;
            }
            out_lines = src_lines * 2;

            for (int src_y = 0; src_y < src_lines; src_y++) {
                const uint16_t *src_row = src_pixels + (((size_t)(row + src_y)) * frame_width);
                uint16_t *dst_row_0 = dst_pixels + (((size_t)(src_y * 2)) * draw_width);
                uint16_t *dst_row_1 = dst_row_0 + draw_width;

                for (int x = 0; x < frame_width; x++) {
                    uint16_t pixel = src_row[x];
                    int dst_x = x * 2;

                    dst_row_0[dst_x] = pixel;
                    dst_row_0[dst_x + 1] = pixel;
                    dst_row_1[dst_x] = pixel;
                    dst_row_1[dst_x + 1] = pixel;
                }
            }
        } else {
            size_t copy_size;

            src_lines = ((row + camera_buf_lines) <= frame_height) ?
                        camera_buf_lines :
                        (frame_height - row);
            out_lines = src_lines;
            copy_size = (size_t)frame_width * src_lines * sizeof(uint16_t);
            memcpy(camera_buf, src + ((size_t)row * frame_width * sizeof(uint16_t)), copy_size);
        }

        xSemaphoreTake(g_lcd_camera.bindings.io_done_sem, 0);
        if (lcd_st7796_draw_bitmap_owned((uint16_t)x_offset,
                                         (uint16_t)(y_offset + (scale_2x ? (row * 2) : row)),
                                         (uint16_t)(x_offset + draw_width),
                                         (uint16_t)(y_offset + (scale_2x ? (row * 2) : row) + out_lines),
                                         camera_buf,
                                         LCD_ST7796_TRANSFER_DIRECT) != ESP_OK) {
            ESP_LOGW(TAG, "camera frame flush failed at row=%d", row);
            break;
        }
        xSemaphoreTake(g_lcd_camera.bindings.io_done_sem, portMAX_DELAY);
    }
}
