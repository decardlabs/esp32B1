#include "app_display.h"
#include "bsp_spi.h"
#include "lcd_st7796.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lvgl.h"

/* Use custom 16px Heiti font (2bpp, compact) for clear Chinese rendering */
LV_FONT_DECLARE(lv_font_xiaozhi_cn_16);

static const char *TAG = "DISPLAY";

static disp_mode_t s_mode = DISP_MODE_INIT;
static char s_filename[64] = {0};
static char s_text[2048] = {0};
static char s_text_render[2048] = {0};
static char s_filename_snapshot[64] = {0};
static char s_text_snapshot[2048] = {0};
static int s_progress_cur = 0;
static int s_progress_total = 0;
static volatile bool s_dirty = false;
static SemaphoreHandle_t s_ui_mutex = NULL;
static StaticSemaphore_t s_ui_mutex_buf;

/* LVGL objects */
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_label_mode = NULL;
static lv_obj_t *s_label_file = NULL;
static lv_obj_t *s_label_text = NULL;
static lv_obj_t *s_label_progress = NULL;
static lv_obj_t *s_label_wifi = NULL;

/* LVGL display buffer (DMA-capable internal RAM only) */
static lv_color_t *s_disp_buf = NULL;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;

static uint32_t utf8_decode_one(const char *src, size_t src_len, size_t *consumed)
{
    if (!src || src_len == 0) {
        *consumed = 0;
        return 0;
    }

    const uint8_t b0 = (uint8_t)src[0];
    if (b0 < 0x80) {
        *consumed = 1;
        return b0;
    }

    if ((b0 & 0xE0) == 0xC0 && src_len >= 2) {
        const uint8_t b1 = (uint8_t)src[1];
        if ((b1 & 0xC0) == 0x80) {
            *consumed = 2;
            return ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
        }
    }

    if ((b0 & 0xF0) == 0xE0 && src_len >= 3) {
        const uint8_t b1 = (uint8_t)src[1];
        const uint8_t b2 = (uint8_t)src[2];
        if (((b1 & 0xC0) == 0x80) && ((b2 & 0xC0) == 0x80)) {
            *consumed = 3;
            return ((uint32_t)(b0 & 0x0F) << 12) |
                   ((uint32_t)(b1 & 0x3F) << 6) |
                   (uint32_t)(b2 & 0x3F);
        }
    }

    if ((b0 & 0xF8) == 0xF0 && src_len >= 4) {
        const uint8_t b1 = (uint8_t)src[1];
        const uint8_t b2 = (uint8_t)src[2];
        const uint8_t b3 = (uint8_t)src[3];
        if (((b1 & 0xC0) == 0x80) && ((b2 & 0xC0) == 0x80) && ((b3 & 0xC0) == 0x80)) {
            *consumed = 4;
            return ((uint32_t)(b0 & 0x07) << 18) |
                   ((uint32_t)(b1 & 0x3F) << 12) |
                   ((uint32_t)(b2 & 0x3F) << 6) |
                   (uint32_t)(b3 & 0x3F);
        }
    }

    /* Invalid UTF-8 lead or sequence: consume one byte safely. */
    *consumed = 1;
    return 0xFFFD;
}

/* Replace unsupported glyphs and control chars to avoid tofu squares. */
static void sanitize_text_for_font(const char *src, char *dst, size_t dst_size, const lv_font_t *font)
{
    if (!src || !dst || dst_size == 0 || !font) return;

    size_t out = 0;
    size_t i = 0;

    while (src[i] != '\0' && out + 1 < dst_size) {
        size_t consumed = 0;
        uint32_t cp = utf8_decode_one(&src[i], strlen(&src[i]), &consumed);
        size_t i_next = i + consumed;

        if (consumed == 0) break;

        /* Normalize CRLF/CR into LF to avoid odd rendering boxes. */
        if (cp == '\r') {
            dst[out++] = '\n';
            i = i_next;
            continue;
        }

        if (cp == '\n' || cp == '\t') {
            dst[out++] = (cp == '\t') ? ' ' : '\n';
            i = i_next;
            continue;
        }

        /* Drop other non-printable controls. */
        if (cp < 0x20) {
            i = i_next;
            continue;
        }

        lv_font_glyph_dsc_t dsc;
        bool has_glyph = lv_font_get_glyph_dsc(font, &dsc, cp, 0);
        if (has_glyph) {
            size_t bytes = (size_t)(i_next - i);
            if (out + bytes >= dst_size) break;
            memcpy(&dst[out], &src[i], bytes);
            out += bytes;
        } else {
            /* Fallback for unsupported chars (emoji/rare CJK/etc.). */
            dst[out++] = '?';
        }

        i = i_next;
    }

    dst[out] = '\0';
}

/* ── LVGL flush callback (called from LVGL when a frame buffer needs flushing) ── */

static bool lcd_lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_disp_drv_t *disp_drv = (lv_disp_drv_t *)user_ctx;
    lcd_st7796_transfer_t transfer = lcd_st7796_take_completed_transfer_from_isr();

    (void)panel_io;
    (void)edata;

    if (transfer == LCD_ST7796_TRANSFER_LVGL && disp_drv) {
        lv_disp_flush_ready(disp_drv);
    }

    return false;
}

static void disp_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_err_t ret = lcd_st7796_draw_bitmap_owned(area->x1, area->y1,
                                                 area->x2 + 1, area->y2 + 1,
                                                 color_map, LCD_ST7796_TRANSFER_LVGL);
    if (ret != ESP_OK) {
        lv_disp_flush_ready(disp_drv);
    }
}

static void lvgl_tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

/* ── Mode string helper ──────────────────────────────────────────────── */

static const char *mode_string(disp_mode_t mode)
{
    switch (mode) {
    case DISP_MODE_INIT:      return "Starting...";
    case DISP_MODE_USB_MSC:   return "USB MSC Mode (U-Disk)";
    case DISP_MODE_SCANNING:  return "Scanning...";
    case DISP_MODE_READING:   return "Reading...";
    case DISP_MODE_PAUSED:    return "Paused";
    case DISP_MODE_FILE_DONE: return "Done";
    case DISP_MODE_CLOUD_READING: return "Cloud Reading...";
    default: return "?";
    }
}

/* ── LVGL task runner (all LVGL API calls MUST be from this task) ────── */

static void lvgl_task(void *arg)
{
    (void)arg;
    uint32_t loop_cnt = 0;
    disp_mode_t mode_snapshot = DISP_MODE_INIT;
    int progress_cur_snapshot = 0;
    int progress_total_snapshot = 0;
    bool dirty_snapshot = false;

    while (1) {
        loop_cnt++;
        if (loop_cnt % 1200 == 0) {
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "[STACK] task=lvgl watermark=%u words", (unsigned)watermark);
        }

        /* Snapshot shared UI state under lock, then render without lock. */
        dirty_snapshot = false;
        if (s_ui_mutex) xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
        if (s_dirty) {
            s_dirty = false;
            mode_snapshot = s_mode;
            progress_cur_snapshot = s_progress_cur;
            progress_total_snapshot = s_progress_total;
            memcpy(s_filename_snapshot, s_filename, sizeof(s_filename_snapshot));
            s_filename_snapshot[sizeof(s_filename_snapshot) - 1] = '\0';
            memcpy(s_text_snapshot, s_text, sizeof(s_text_snapshot));
            s_text_snapshot[sizeof(s_text_snapshot) - 1] = '\0';
            dirty_snapshot = true;
        }
        if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);

        /* Check if display data was updated from another task */
        if (dirty_snapshot) {
            lv_label_set_text(s_label_mode, mode_string(mode_snapshot));

            if (s_filename_snapshot[0]) {
                lv_label_set_text(s_label_file, s_filename_snapshot);
                lv_obj_set_style_text_align(s_label_file, LV_TEXT_ALIGN_CENTER, 0);
            } else {
                lv_label_set_text(s_label_file, "");
            }

            if (s_text_snapshot[0]) {
                sanitize_text_for_font(s_text_snapshot, s_text_render, sizeof(s_text_render), &lv_font_xiaozhi_cn_16);
                lv_label_set_text(s_label_text, s_text_render);
            } else {
                lv_label_set_text(s_label_text, "(Empty file)");
            }

            if (progress_total_snapshot > 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d / %d", progress_cur_snapshot, progress_total_snapshot);
                lv_label_set_text(s_label_progress, buf);
            } else {
                lv_label_set_text(s_label_progress, "");
            }

            /* WiFi indicator */
            {
                extern bool app_wifi_is_connected(void);
                lv_label_set_text(s_label_wifi, app_wifi_is_connected() ? "[WiFi]" : "");
            }
        }

        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms < 5) wait_ms = 5;
        if (wait_ms > 20) wait_ms = 20;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

/* ── UI setup ────────────────────────────────────────────────────────── */

static void create_ui(void)
{
    s_screen = lv_scr_act();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x1565C0), 0);

    /* Mode indicator (top left) */
    s_label_mode = lv_label_create(s_screen);
    lv_obj_align(s_label_mode, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_label_set_text(s_label_mode, "Starting...");
    /* Use larger font for mode indicator */
    static lv_style_t style_mode;
    lv_style_init(&style_mode);
    lv_style_set_text_font(&style_mode, &lv_font_montserrat_16);
    lv_style_set_text_color(&style_mode, lv_color_hex(0xBBDEFB));
    lv_obj_add_style(s_label_mode, &style_mode, 0);

    /* Centered filename (top area) */
    s_label_file = lv_label_create(s_screen);
    lv_obj_set_width(s_label_file, LCD_H_RES - 20);
    lv_obj_set_style_text_align(s_label_file, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_label_file, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(s_label_file, "");

    /* Separator line */
    lv_obj_t *line = lv_obj_create(s_screen);
    lv_obj_set_size(line, LCD_H_RES - 20, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x4080C0), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 70);

    /* Scrollable text area (ebook body) */
    s_label_text = lv_label_create(s_screen);
    lv_obj_set_width(s_label_text, LCD_H_RES - 30);
    lv_obj_set_pos(s_label_text, 15, 80);
    lv_obj_set_height(s_label_text, LCD_V_RES - 120);
    lv_label_set_long_mode(s_label_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_label_text, "Ready.");
    lv_obj_set_style_text_color(s_label_text, lv_color_hex(0xFFFFFF), 0);

    /* Progress indicator (bottom) */
    s_label_progress = lv_label_create(s_screen);
    lv_obj_align(s_label_progress, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_label_set_text(s_label_progress, "");

    /* WiFi status (top right) */
    s_label_wifi = lv_label_create(s_screen);
    lv_obj_align(s_label_wifi, LV_ALIGN_TOP_RIGHT, -5, 5);
    lv_label_set_text(s_label_wifi, "");
    static lv_style_t style_wifi;
    lv_style_init(&style_wifi);
    lv_style_set_text_font(&style_wifi, &lv_font_montserrat_14);
    lv_style_set_text_color(&style_wifi, lv_color_hex(0x81C784));
    lv_obj_add_style(s_label_wifi, &style_wifi, 0);

    /* Style for text (16px Heiti, sharp on LCD) */
    static lv_style_t style_text;
    lv_style_init(&style_text);
    lv_style_set_text_font(&style_text, &lv_font_xiaozhi_cn_16);
    lv_style_set_text_color(&style_text, lv_color_hex(0xFFFFFF));
    lv_style_set_text_line_space(&style_text, 4);
    lv_obj_add_style(s_label_text, &style_text, 0);
    lv_obj_add_style(s_label_file, &style_text, 0);
}

/* ── Update helpers ──────────────────────────────────────────────────── */

void app_display_set_mode(disp_mode_t mode)
{
    if (s_ui_mutex) xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_mode = mode;
    s_dirty = true;
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}

void app_display_set_filename(const char *name)
{
    if (s_ui_mutex) xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    strncpy(s_filename, name ? name : "", sizeof(s_filename) - 1);
    s_filename[sizeof(s_filename) - 1] = '\0';
    s_dirty = true;
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}

void app_display_set_text(const char *text)
{
    if (s_ui_mutex) xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    strncpy(s_text, text ? text : "", sizeof(s_text) - 1);
    s_text[sizeof(s_text) - 1] = '\0';
    s_dirty = true;
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}

void app_display_set_mode_text(disp_mode_t mode, const char *text)
{
    if (s_ui_mutex) xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_mode = mode;
    if (text) {
        strncpy(s_text, text, sizeof(s_text) - 1);
        s_text[sizeof(s_text) - 1] = '\0';
    }
    s_dirty = true;
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}

void app_display_set_progress(int current, int total)
{
    if (s_ui_mutex) xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_progress_cur = current;
    s_progress_total = total;
    s_dirty = true;
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}

void app_display_set_wifi_status(bool connected)
{
    (void)connected;
    if (s_ui_mutex) xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_dirty = true;
    if (s_ui_mutex) xSemaphoreGive(s_ui_mutex);
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t app_display_init(void)
{
    /* Init SPI2 bus */
    ESP_ERROR_CHECK(bsp_spi2_lcd_init());

    /* Init LCD panel via esp_lcd_panel_io */
    ESP_ERROR_CHECK(lcd_st7796_init());

    /* Allocate DMA-capable LVGL draw buffer from internal RAM.
     * DO NOT fall back to PSRAM — DMA buffers cannot be in PSRAM (§1.3). */
    size_t buf_size = LCD_ST7796_H_RES * LCD_ST7796_BUF_LINES * sizeof(lv_color_t);
    s_disp_buf = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_disp_buf) {
        ESP_LOGE(TAG, "Failed to allocate DMA-capable LVGL draw buffer (%zu bytes)", buf_size);
        return ESP_ERR_NO_MEM;
    }

    /* Init LVGL */
    lv_init();

    if (!s_ui_mutex) {
        s_ui_mutex = xSemaphoreCreateMutexStatic(&s_ui_mutex_buf);
        if (!s_ui_mutex) {
            ESP_LOGE(TAG, "Failed to create UI state mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Register LCD flush ISR callback */
    ESP_ERROR_CHECK(lcd_st7796_register_event_callbacks(&(esp_lcd_panel_io_callbacks_t){
        .on_color_trans_done = lcd_lvgl_flush_ready_cb,
    }, &s_disp_drv));

    lv_disp_draw_buf_init(&s_draw_buf, s_disp_buf, NULL,
                          LCD_ST7796_H_RES * LCD_ST7796_BUF_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_ST7796_H_RES;
    s_disp_drv.ver_res = LCD_ST7796_V_RES;
    s_disp_drv.flush_cb = disp_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    /* Create LVGL UI */
    create_ui();

    /* Start LVGL tick timer (2ms interval) */
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_timer_cb,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_timer_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 2000);

    /* Start LVGL task (8KB stack for LVGL + local text buffers) */
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);

    /* Give LVGL time to render initial UI */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Set initial mode - will be overwritten by app_main init steps */
    s_mode = DISP_MODE_USB_MSC;
    app_display_set_filename("Voice Reader");
    app_display_set_text("Booting...");

    ESP_LOGI(TAG, "LVGL display ready");
    return ESP_OK;
}
