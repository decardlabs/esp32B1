#include "task_lcd_lvgl.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lcd_st7796.h"
#include "lvgl.h"

#define LCD_LVGL_TICK_MS         2
#define LCD_LVGL_HANDLER_MIN_MS  5
#define LCD_LVGL_HANDLER_MAX_MS  20
#define LCD_LVGL_TASK_STACK      8192
#define LCD_LVGL_UI_TIMER_MS     150

#define LCD_UI_STATUS_LEN        96
#define LCD_UI_RECORD_LEN        256
#define LCD_UI_REPLY_LEN         768

LV_FONT_DECLARE(lv_font_simhei_16_gb2312);
#define LCD_UI_FONT              (&lv_font_simhei_16_gb2312)

static const char *TAG = "TASK_LCD";

static lv_disp_draw_buf_t lcd_draw_buf;
static lv_disp_drv_t lcd_disp_drv;
static lv_color_t *lcd_buf_1 = NULL;
static esp_timer_handle_t lcd_lvgl_tick_timer = NULL;

static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_record_label = NULL;
static lv_obj_t *s_reply_label = NULL;

static SemaphoreHandle_t s_ui_mutex = NULL;
static bool s_ui_dirty = true;
static char s_ui_status[LCD_UI_STATUS_LEN] = "正在启动...";
static char s_ui_record[LCD_UI_RECORD_LEN] = "按住 KEY1 后开始说话。";
static char s_ui_reply[LCD_UI_REPLY_LEN] = "小智的回复会显示在这里。";

static void lcd_lvgl_ui_timer_cb(lv_timer_t *timer);

static bool lcd_lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_disp_drv_t *disp_drv = user_ctx;
    lcd_st7796_transfer_t transfer = lcd_st7796_take_completed_transfer_from_isr();

    (void)panel_io;
    (void)edata;

    if ((transfer == LCD_ST7796_TRANSFER_LVGL) && (disp_drv != NULL)) {
        lv_disp_flush_ready(disp_drv);
    }

    return false;
}

static void lcd_lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LCD_LVGL_TICK_MS);
}

static void lcd_lvgl_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_err_t ret = lcd_st7796_draw_bitmap_owned(area->x1,
                                                 area->y1,
                                                 area->x2 + 1,
                                                 area->y2 + 1,
                                                 color_map,
                                                 LCD_ST7796_TRANSFER_LVGL);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lcd flush failed: %s", esp_err_to_name(ret));
        lv_disp_flush_ready(disp_drv);
    }
}

static void lcd_lvgl_ui_mutex_init(void)
{
    if (s_ui_mutex == NULL) {
        s_ui_mutex = xSemaphoreCreateMutex();
    }
}

static void lcd_lvgl_copy_text(char *dst, size_t dst_size, const char *src)
{
    if ((dst == NULL) || (dst_size == 0)) {
        return;
    }

    if (src == NULL) {
        src = "";
    }

    strlcpy(dst, src, dst_size);
}

static lv_obj_t *lcd_lvgl_create_title(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, LCD_UI_FONT, 0);
    return label;
}

static lv_obj_t *lcd_lvgl_create_text_box(lv_obj_t *parent, int height)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_set_width(box, 288);
    lv_obj_set_height(box, height);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x2E4D5F), 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x17252F), 0);
    lv_obj_set_style_pad_all(box, 10, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static void lcd_lvgl_xiaozhi_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *title;
    lv_obj_t *hint;
    lv_obj_t *status_box;
    lv_obj_t *record_title;
    lv_obj_t *record_box;
    lv_obj_t *reply_title;
    lv_obj_t *reply_box;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E171C), 0);
    lv_obj_set_style_text_font(scr, LCD_UI_FONT, 0);

    title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32 小智");
    lv_obj_set_style_text_color(title, lv_color_hex(0xEAF7F3), 0);
    lv_obj_set_style_text_font(title, LCD_UI_FONT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    hint = lv_label_create(scr);
    lv_label_set_text(hint, "按住 KEY1 说话");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x76D7C4), 0);
    lv_obj_set_style_text_font(hint, LCD_UI_FONT, 0);
    lv_obj_align_to(hint, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    status_box = lcd_lvgl_create_text_box(scr, 54);
    lv_obj_align(status_box, LV_ALIGN_TOP_MID, 0, 72);

    s_status_label = lv_label_create(status_box);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_status_label, 260);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xF9F7E8), 0);
    lv_obj_set_style_text_font(s_status_label, LCD_UI_FONT, 0);
    lv_label_set_text(s_status_label, s_ui_status);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);

    record_title = lcd_lvgl_create_title(scr, "识别内容", lv_color_hex(0x98D6FF));
    lv_obj_align(record_title, LV_ALIGN_TOP_LEFT, 18, 144);

    record_box = lcd_lvgl_create_text_box(scr, 118);
    lv_obj_align(record_box, LV_ALIGN_TOP_MID, 0, 170);

    s_record_label = lv_label_create(record_box);
    lv_label_set_long_mode(s_record_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_record_label, 264);
    lv_obj_set_style_text_color(s_record_label, lv_color_hex(0xDDECF3), 0);
    lv_obj_set_style_text_font(s_record_label, LCD_UI_FONT, 0);
    lv_label_set_text(s_record_label, s_ui_record);
    lv_obj_align(s_record_label, LV_ALIGN_TOP_LEFT, 0, 0);

    reply_title = lcd_lvgl_create_title(scr, "小智回复", lv_color_hex(0xFFD166));
    lv_obj_align(reply_title, LV_ALIGN_TOP_LEFT, 18, 306);

    reply_box = lcd_lvgl_create_text_box(scr, 140);
    lv_obj_align(reply_box, LV_ALIGN_TOP_MID, 0, 332);

    s_reply_label = lv_label_create(reply_box);
    lv_label_set_long_mode(s_reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_reply_label, 264);
    lv_obj_set_style_text_color(s_reply_label, lv_color_hex(0xFFF7DE), 0);
    lv_obj_set_style_text_font(s_reply_label, LCD_UI_FONT, 0);
    lv_label_set_text(s_reply_label, s_ui_reply);
    lv_obj_align(s_reply_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_timer_create(lcd_lvgl_ui_timer_cb, LCD_LVGL_UI_TIMER_MS, NULL);
}

static void lcd_lvgl_ui_timer_cb(lv_timer_t *timer)
{
    char status[LCD_UI_STATUS_LEN];
    char record[LCD_UI_RECORD_LEN];
    char reply[LCD_UI_REPLY_LEN];
    bool dirty = false;

    (void)timer;

    if (s_ui_mutex != NULL) {
        xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
        dirty = s_ui_dirty;
        if (dirty) {
            lcd_lvgl_copy_text(status, sizeof(status), s_ui_status);
            lcd_lvgl_copy_text(record, sizeof(record), s_ui_record);
            lcd_lvgl_copy_text(reply, sizeof(reply), s_ui_reply);
            s_ui_dirty = false;
        }
        xSemaphoreGive(s_ui_mutex);
    }

    if (!dirty) {
        return;
    }

    lv_label_set_text(s_status_label, status);
    lv_label_set_text(s_record_label, record);
    lv_label_set_text(s_reply_label, reply);
}

esp_err_t lcd_lvgl_reserve_buffer(void)
{
    if (lcd_buf_1 != NULL) {
        return ESP_OK;
    }

    lcd_buf_1 = heap_caps_malloc(LCD_ST7796_H_RES * LCD_ST7796_BUF_LINES * sizeof(lv_color_t),
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (lcd_buf_1 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void lcd_lvgl_set_status(const char *text)
{
    lcd_lvgl_ui_mutex_init();
    if (s_ui_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    lcd_lvgl_copy_text(s_ui_status, sizeof(s_ui_status), text);
    s_ui_dirty = true;
    xSemaphoreGive(s_ui_mutex);
}

void lcd_lvgl_set_record_text(const char *text)
{
    lcd_lvgl_ui_mutex_init();
    if (s_ui_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    lcd_lvgl_copy_text(s_ui_record, sizeof(s_ui_record), text);
    s_ui_dirty = true;
    xSemaphoreGive(s_ui_mutex);
}

void lcd_lvgl_set_reply_text(const char *text)
{
    lcd_lvgl_ui_mutex_init();
    if (s_ui_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    lcd_lvgl_copy_text(s_ui_reply, sizeof(s_ui_reply), text);
    s_ui_dirty = true;
    xSemaphoreGive(s_ui_mutex);
}

static void lcd_lvgl_task(void *pvParameters)
{
    lv_disp_t *disp;
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lcd_lvgl_tick_cb,
        .name = "lvgl_tick",
    };

    (void)pvParameters;

    lcd_lvgl_ui_mutex_init();
    ESP_ERROR_CHECK(lcd_lvgl_reserve_buffer());

    lv_init();
    ESP_ERROR_CHECK(lcd_st7796_register_event_callbacks(&(esp_lcd_panel_io_callbacks_t) {
        .on_color_trans_done = lcd_lvgl_flush_ready_cb,
    }, &lcd_disp_drv));

    lv_disp_draw_buf_init(&lcd_draw_buf, lcd_buf_1, NULL, LCD_ST7796_H_RES * LCD_ST7796_BUF_LINES);

    lv_disp_drv_init(&lcd_disp_drv);
    lcd_disp_drv.hor_res = LCD_ST7796_H_RES;
    lcd_disp_drv.ver_res = LCD_ST7796_V_RES;
    lcd_disp_drv.flush_cb = lcd_lvgl_flush_cb;
    lcd_disp_drv.draw_buf = &lcd_draw_buf;
    disp = lv_disp_drv_register(&lcd_disp_drv);
    lv_disp_set_default(disp);

    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &lcd_lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lcd_lvgl_tick_timer, LCD_LVGL_TICK_MS * 1000));

    lcd_lvgl_xiaozhi_ui_create();
    ESP_LOGI(TAG, "xiaozhi lvgl ui start");

    while (1) {
        uint32_t wait_ms = lv_timer_handler();

        if (wait_ms < LCD_LVGL_HANDLER_MIN_MS) {
            wait_ms = LCD_LVGL_HANDLER_MIN_MS;
        } else if (wait_ms > LCD_LVGL_HANDLER_MAX_MS) {
            wait_ms = LCD_LVGL_HANDLER_MAX_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

BaseType_t lcd_lvgl_task_create(void)
{
    return xTaskCreate(lcd_lvgl_task, "lcd_lvgl_task", LCD_LVGL_TASK_STACK, NULL, 6, NULL);
}
