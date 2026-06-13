#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include "task_lcd_lvgl.h"
#include "task_lcd_lvgl_camera.h"
#include "task_xiaozhi.h"
#include "lcd_st7796.h"
#include "esp_heap_caps.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "xl9555.h"
#include "xiaozhi_ui_status.h"

LV_FONT_DECLARE(lv_font_xiaozhi_cn_16);

#define LCD_LVGL_TICK_MS         2
#define LCD_LVGL_HANDLER_MIN_MS  10
#define LCD_LVGL_HANDLER_MAX_MS  30
#define LCD_LVGL_UPDATE_MS       120
#define LCD_LVGL_TASK_STACK      8192
#define LCD_LVGL_BOOT_DELAY_MS   80
#define LCD_LVGL_BUF_LINES       20
#define LCD_LVGL_WAVE_COUNT      3
#define LCD_LVGL_FONT_MAIN       (&lv_font_xiaozhi_cn_16)
#define LCD_LVGL_SWITCH_KEY_MS   20
#define LCD_LVGL_SWITCH_DEBOUNCE_MS 40
#define LCD_LVGL_ORB_STAGE_SIZE  138
#define LCD_LVGL_ORB_GLOW_BASE   112
#define LCD_LVGL_ORB_RING_BASE   88
#define LCD_LVGL_ORB_CORE_BASE   64
#define LCD_LVGL_BAR_WIDTH       10
#define LCD_CAMERA_FRAME_DELAY_MS  24
#define LCD_VOICE_PANEL_WIDTH    288
#define LCD_VOICE_LIGHT_HEIGHT   38
#define LCD_VOICE_STT_HEIGHT     58
#define LCD_VOICE_TTS_HEIGHT     78
#define LCD_SETUP_INFO_LEFT_PAD  14
#define LCD_SETUP_INFO_RIGHT_PAD 14
#define LCD_SETUP_INFO_VALUE_GAP 10
#define LCD_SETUP_INFO_MIN_VALUE_WIDTH 120
#define LCD_CAMERA_INFO_HEIGHT   88

typedef struct {
    lv_color_t accent;
    lv_color_t accent_soft;
    const char *phase_text;
    const char *status_text;
} lcd_lvgl_theme_t;

typedef struct {
    lv_obj_t *setup_view;
    lv_obj_t *voice_view;
    lv_obj_t *camera_view;
    lv_obj_t *setup_title_label;
    lv_obj_t *setup_status_label;
    lv_obj_t *setup_wifi_title_label;
    lv_obj_t *setup_wifi_text_label;
    lv_obj_t *setup_ip_title_label;
    lv_obj_t *setup_ip_text_label;
    lv_obj_t *setup_binding_title_label;
    lv_obj_t *setup_binding_text_label;
    lv_obj_t *setup_hint_label;
    lv_obj_t *title_label;
    lv_obj_t *phase_label;
    lv_obj_t *status_label;
    lv_obj_t *light_title_label;
    lv_obj_t *light_text_label;
    lv_obj_t *stt_title_label;
    lv_obj_t *stt_text_label;
    lv_obj_t *tts_title_label;
    lv_obj_t *tts_text_label;
    lv_obj_t *camera_title_label;
    lv_obj_t *camera_note_label;
    lv_obj_t *camera_hint_label;
    lv_obj_t *orb_glow;
    lv_obj_t *orb_ring;
    lv_obj_t *orb_core;
    lv_obj_t *bars[LCD_LVGL_WAVE_COUNT];
    int anim_tick;
    xiaozhi_ui_snapshot_t last_snapshot;
    bool snapshot_valid;
} lcd_lvgl_ui_t;

typedef struct {
    bool key2_raw_pressed;
    bool key2_stable_pressed;
    TickType_t key2_last_change_tick;
    xiaozhi_ui_view_t active_view;
} lcd_lvgl_runtime_t;

static const char *TAG = "TASK_LCD";

static lv_disp_draw_buf_t lcd_draw_buf;
static lv_disp_drv_t lcd_disp_drv;
static lv_color_t *lcd_buf_1 = NULL;
static esp_timer_handle_t lcd_lvgl_tick_timer = NULL;
static SemaphoreHandle_t lcd_io_done_sem = NULL;
static lcd_lvgl_ui_t g_lcd_ui = {0};
static lcd_lvgl_runtime_t g_lcd_runtime = {
    .active_view = XIAOZHI_UI_VIEW_SETUP,
};

static bool lcd_lvgl_flush_ready_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_drv = user_ctx;
    lcd_st7796_transfer_t transfer = lcd_st7796_take_completed_transfer_from_isr();
    BaseType_t high_task_wakeup = pdFALSE;
    bool need_yield = false;

    (void)panel_io;
    (void)edata;

    if ((transfer == LCD_ST7796_TRANSFER_DIRECT) && (lcd_io_done_sem != NULL)) {
        xSemaphoreGiveFromISR(lcd_io_done_sem, &high_task_wakeup);
        need_yield = (high_task_wakeup == pdTRUE);
    } else if ((transfer == LCD_ST7796_TRANSFER_LVGL) && (disp_drv != NULL)) {
        lv_disp_flush_ready(disp_drv);
    }

    return need_yield;
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

static void lcd_lvgl_set_obj_hidden(lv_obj_t *obj, bool hidden)
{
    if (obj == NULL) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lcd_lvgl_apply_view(xiaozhi_ui_view_t view)
{
    lcd_lvgl_set_obj_hidden(g_lcd_ui.setup_view, view != XIAOZHI_UI_VIEW_SETUP);
    lcd_lvgl_set_obj_hidden(g_lcd_ui.voice_view, view != XIAOZHI_UI_VIEW_VOICE);
    lcd_lvgl_set_obj_hidden(g_lcd_ui.camera_view, view != XIAOZHI_UI_VIEW_CAMERA);
}

static bool lcd_lvgl_key2_is_pressed(void)
{
    bool key_level = true;

    xl9555_get_pin_level(KEY_PORT, KEY2_PIN, &key_level);
    return (key_level == 0);
}

static void lcd_lvgl_switch_key_init(void)
{
    g_lcd_runtime.key2_last_change_tick = xTaskGetTickCount();
}

static void lcd_lvgl_poll_switch_key(void)
{
    TickType_t debounce_ticks = pdMS_TO_TICKS(LCD_LVGL_SWITCH_DEBOUNCE_MS);
    TickType_t now = xTaskGetTickCount();
    bool level_low = lcd_lvgl_key2_is_pressed();

    if (level_low != g_lcd_runtime.key2_raw_pressed) {
        g_lcd_runtime.key2_raw_pressed = level_low;
        g_lcd_runtime.key2_last_change_tick = now;
    }

    if (((now - g_lcd_runtime.key2_last_change_tick) >= debounce_ticks) &&
        (g_lcd_runtime.key2_stable_pressed != g_lcd_runtime.key2_raw_pressed)) {
        g_lcd_runtime.key2_stable_pressed = g_lcd_runtime.key2_raw_pressed;

        if (g_lcd_runtime.key2_stable_pressed) {
            xiaozhi_ui_view_t next_view = (xiaozhi_ui_status_get_view() == XIAOZHI_UI_VIEW_CAMERA) ?
                                         XIAOZHI_UI_VIEW_VOICE :
                                         XIAOZHI_UI_VIEW_CAMERA;

            if (next_view == XIAOZHI_UI_VIEW_CAMERA) {
                xiaozhi_enter_camera_view();
            }

            xiaozhi_ui_status_set_view(next_view);
            ESP_LOGI(TAG, "switch view -> %s", (next_view == XIAOZHI_UI_VIEW_CAMERA) ? "camera" : "voice");
        }
    }
}

static int lcd_lvgl_triangle_wave(int tick, int period)
{
    int value;

    if (period <= 1) {
        return 0;
    }

    value = tick % period;
    if (value > (period / 2)) {
        value = period - value;
    }

    return value;
}

static bool lcd_lvgl_phase_is_animated(xiaozhi_ui_phase_t phase)
{
    return ((phase == XIAOZHI_UI_PHASE_LISTENING) ||
            (phase == XIAOZHI_UI_PHASE_THINKING) ||
            (phase == XIAOZHI_UI_PHASE_SPEAKING));
}

static lcd_lvgl_theme_t lcd_lvgl_get_theme(xiaozhi_ui_phase_t phase)
{
    switch (phase) {
        case XIAOZHI_UI_PHASE_CONNECTING:
            return (lcd_lvgl_theme_t) {
                .accent = lv_color_hex(0x57C7FF),
                .accent_soft = lv_color_hex(0xA8E1FF),
                .phase_text = "连接中",
                .status_text = "正在建立语音链路",
            };

        case XIAOZHI_UI_PHASE_READY:
            return (lcd_lvgl_theme_t) {
                .accent = lv_color_hex(0x67E8F9),
                .accent_soft = lv_color_hex(0xC4F8FF),
                .phase_text = "就绪",
                .status_text = "按住按键开始说话",
            };

        case XIAOZHI_UI_PHASE_LISTENING:
            return (lcd_lvgl_theme_t) {
                .accent = lv_color_hex(0x63A7FF),
                .accent_soft = lv_color_hex(0xC8DEFF),
                .phase_text = "聆听中",
                .status_text = "松开发送给小智",
            };

        case XIAOZHI_UI_PHASE_THINKING:
            return (lcd_lvgl_theme_t) {
                .accent = lv_color_hex(0xF5C36B),
                .accent_soft = lv_color_hex(0xFFE2A8),
                .phase_text = "思考中",
                .status_text = "正在生成回答",
            };

        case XIAOZHI_UI_PHASE_SPEAKING:
            return (lcd_lvgl_theme_t) {
                .accent = lv_color_hex(0x63E59C),
                .accent_soft = lv_color_hex(0xBDF8D1),
                .phase_text = "播报中",
                .status_text = "正在播放语音回复",
            };

        case XIAOZHI_UI_PHASE_DISCONNECTED:
            return (lcd_lvgl_theme_t) {
                .accent = lv_color_hex(0xFF8CA3),
                .accent_soft = lv_color_hex(0xFFD1DA),
                .phase_text = "重连中",
                .status_text = "等待重新连接服务器",
            };

        case XIAOZHI_UI_PHASE_ERROR:
            return (lcd_lvgl_theme_t) {
                .accent = lv_color_hex(0xFF738E),
                .accent_soft = lv_color_hex(0xFFC3CF),
                .phase_text = "异常",
                .status_text = "语音链路异常",
            };

        case XIAOZHI_UI_PHASE_BOOT:
        default:
            return (lcd_lvgl_theme_t) {
                .accent = lv_color_hex(0xB18CFF),
                .accent_soft = lv_color_hex(0xD7C2FF),
                .phase_text = "启动中",
                .status_text = "正在启动小智",
            };
    }
}

static void lcd_lvgl_style_panel(lv_obj_t *panel)
{
    lv_obj_set_style_radius(panel, 20, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x232B3C), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x0C1018), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
}

static void lcd_lvgl_center_panel_stack(lv_obj_t *panel, lv_obj_t *title_label, lv_obj_t *text_label, lv_coord_t gap)
{
    if ((panel == NULL) || (title_label == NULL) || (text_label == NULL)) {
        return;
    }

    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(panel, 12, 0);
    lv_obj_set_style_pad_right(panel, 12, 0);
    lv_obj_set_style_pad_top(panel, 0, 0);
    lv_obj_set_style_pad_bottom(panel, 0, 0);
    lv_obj_set_style_pad_row(panel, gap, 0);
    lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_LEFT, 0);
}

static void lcd_lvgl_align_setup_info_row(lv_obj_t *panel,
                                          lv_obj_t *title_label,
                                          lv_obj_t *text_label,
                                          lv_coord_t top)
{
    lv_coord_t value_x;
    lv_coord_t value_width;

    if ((panel == NULL) || (title_label == NULL) || (text_label == NULL)) {
        return;
    }

    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, LCD_SETUP_INFO_LEFT_PAD, top);
    lv_obj_update_layout(panel);

    value_x = lv_obj_get_x(title_label) + lv_obj_get_width(title_label) + LCD_SETUP_INFO_VALUE_GAP;
    value_width = lv_obj_get_width(panel) - value_x - LCD_SETUP_INFO_RIGHT_PAD;
    if (value_width < LCD_SETUP_INFO_MIN_VALUE_WIDTH) {
        value_width = LCD_SETUP_INFO_MIN_VALUE_WIDTH;
    }

    lv_obj_set_width(text_label, value_width);
    lv_obj_align(text_label, LV_ALIGN_TOP_LEFT, value_x, top);
}

static void lcd_lvgl_build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *setup_info_panel;
    lv_obj_t *orb_stage;
    lv_obj_t *wave_stage;
    lv_obj_t *light_panel;
    lv_obj_t *stt_panel;
    lv_obj_t *tts_panel;
    lv_obj_t *camera_info_panel;
    lv_obj_t *camera_info_title;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05050A), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x05050A), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);

    g_lcd_ui.setup_view = lv_obj_create(scr);
    lv_obj_remove_style_all(g_lcd_ui.setup_view);
    lv_obj_set_size(g_lcd_ui.setup_view, LCD_ST7796_H_RES, LCD_ST7796_V_RES);
    lv_obj_center(g_lcd_ui.setup_view);
    lv_obj_set_style_bg_opa(g_lcd_ui.setup_view, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_lcd_ui.setup_view, LV_OBJ_FLAG_SCROLLABLE);

    g_lcd_ui.voice_view = lv_obj_create(scr);
    lv_obj_remove_style_all(g_lcd_ui.voice_view);
    lv_obj_set_size(g_lcd_ui.voice_view, LCD_ST7796_H_RES, LCD_ST7796_V_RES);
    lv_obj_center(g_lcd_ui.voice_view);
    lv_obj_set_style_bg_opa(g_lcd_ui.voice_view, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_lcd_ui.voice_view, LV_OBJ_FLAG_SCROLLABLE);

    g_lcd_ui.camera_view = lv_obj_create(scr);
    lv_obj_remove_style_all(g_lcd_ui.camera_view);
    lv_obj_set_size(g_lcd_ui.camera_view, LCD_ST7796_H_RES, LCD_ST7796_V_RES);
    lv_obj_center(g_lcd_ui.camera_view);
    lv_obj_set_style_bg_opa(g_lcd_ui.camera_view, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_lcd_ui.camera_view, LV_OBJ_FLAG_SCROLLABLE);

    g_lcd_ui.setup_title_label = lv_label_create(g_lcd_ui.setup_view);
    lv_label_set_text(g_lcd_ui.setup_title_label, "联网 / 绑定");
    lv_obj_set_style_text_font(g_lcd_ui.setup_title_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.setup_title_label, lv_color_hex(0xD9E2F0), 0);
    lv_obj_align(g_lcd_ui.setup_title_label, LV_ALIGN_TOP_LEFT, 18, 16);

    g_lcd_ui.setup_status_label = lv_label_create(g_lcd_ui.setup_view);
    lv_obj_set_size(g_lcd_ui.setup_status_label, 268, 52);
    lv_label_set_long_mode(g_lcd_ui.setup_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_lcd_ui.setup_status_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_align(g_lcd_ui.setup_status_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(g_lcd_ui.setup_status_label, lv_color_hex(0xA8B6C9), 0);
    lv_label_set_text(g_lcd_ui.setup_status_label, "正在准备联网和绑定信息");
    lv_obj_align(g_lcd_ui.setup_status_label, LV_ALIGN_TOP_LEFT, 18, 48);

    setup_info_panel = lv_obj_create(g_lcd_ui.setup_view);
    lv_obj_set_size(setup_info_panel, LCD_VOICE_PANEL_WIDTH, 94);
    lv_obj_align_to(setup_info_panel, g_lcd_ui.setup_status_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    lcd_lvgl_style_panel(setup_info_panel);

    g_lcd_ui.setup_wifi_title_label = lv_label_create(setup_info_panel);
    lv_label_set_text(g_lcd_ui.setup_wifi_title_label, "Wi-Fi");
    lv_obj_set_style_text_font(g_lcd_ui.setup_wifi_title_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.setup_wifi_title_label, lv_color_hex(0x8FA3C2), 0);

    g_lcd_ui.setup_wifi_text_label = lv_label_create(setup_info_panel);
    lv_label_set_long_mode(g_lcd_ui.setup_wifi_text_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_lcd_ui.setup_wifi_text_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.setup_wifi_text_label, lv_color_hex(0xE4ECF8), 0);
    lv_label_set_text(g_lcd_ui.setup_wifi_text_label, "未连接");
    lcd_lvgl_align_setup_info_row(setup_info_panel,
                                  g_lcd_ui.setup_wifi_title_label,
                                  g_lcd_ui.setup_wifi_text_label,
                                  12);

    g_lcd_ui.setup_ip_title_label = lv_label_create(setup_info_panel);
    lv_label_set_text(g_lcd_ui.setup_ip_title_label, "网址 / IP");
    lv_obj_set_style_text_font(g_lcd_ui.setup_ip_title_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.setup_ip_title_label, lv_color_hex(0x8FA3C2), 0);

    g_lcd_ui.setup_ip_text_label = lv_label_create(setup_info_panel);
    lv_label_set_long_mode(g_lcd_ui.setup_ip_text_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_lcd_ui.setup_ip_text_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.setup_ip_text_label, lv_color_hex(0xE4ECF8), 0);
    lv_label_set_text(g_lcd_ui.setup_ip_text_label, "--");
    lcd_lvgl_align_setup_info_row(setup_info_panel,
                                  g_lcd_ui.setup_ip_title_label,
                                  g_lcd_ui.setup_ip_text_label,
                                  38);

    g_lcd_ui.setup_binding_title_label = lv_label_create(setup_info_panel);
    lv_label_set_text(g_lcd_ui.setup_binding_title_label, "绑定码");
    lv_obj_set_style_text_font(g_lcd_ui.setup_binding_title_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.setup_binding_title_label, lv_color_hex(0x8FA3C2), 0);

    g_lcd_ui.setup_binding_text_label = lv_label_create(setup_info_panel);
    lv_label_set_long_mode(g_lcd_ui.setup_binding_text_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_lcd_ui.setup_binding_text_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.setup_binding_text_label, lv_color_hex(0xE4ECF8), 0);
    lv_label_set_text(g_lcd_ui.setup_binding_text_label, "查询中");
    lcd_lvgl_align_setup_info_row(setup_info_panel,
                                  g_lcd_ui.setup_binding_title_label,
                                  g_lcd_ui.setup_binding_text_label,
                                  64);

    g_lcd_ui.setup_hint_label = lv_label_create(g_lcd_ui.setup_view);
    lv_obj_set_size(g_lcd_ui.setup_hint_label, 268, 52);
    lv_label_set_long_mode(g_lcd_ui.setup_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_lcd_ui.setup_hint_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_align(g_lcd_ui.setup_hint_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(g_lcd_ui.setup_hint_label, lv_color_hex(0x8FA3C2), 0);
    lv_label_set_text(g_lcd_ui.setup_hint_label,
                      "如果还没联网，先连上方 Wi-Fi / 热点，再打开上方网址。\n完成联网和设备绑定后，会自动进入小智语音界面。");
    lv_obj_align_to(g_lcd_ui.setup_hint_label, setup_info_panel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 14);

    g_lcd_ui.title_label = lv_label_create(g_lcd_ui.voice_view);
    lv_label_set_text(g_lcd_ui.title_label, "小智");
    lv_obj_set_style_text_font(g_lcd_ui.title_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.title_label, lv_color_hex(0xD9E2F0), 0);
    lv_obj_align(g_lcd_ui.title_label, LV_ALIGN_TOP_LEFT, 18, 16);

    orb_stage = lv_obj_create(g_lcd_ui.voice_view);
    lv_obj_remove_style_all(orb_stage);
    lv_obj_set_size(orb_stage, LCD_LVGL_ORB_STAGE_SIZE, LCD_LVGL_ORB_STAGE_SIZE);
    lv_obj_align(orb_stage, LV_ALIGN_TOP_MID, 0, 28);

    g_lcd_ui.orb_glow = lv_obj_create(orb_stage);
    lv_obj_remove_style_all(g_lcd_ui.orb_glow);
    lv_obj_set_size(g_lcd_ui.orb_glow, LCD_LVGL_ORB_GLOW_BASE, LCD_LVGL_ORB_GLOW_BASE);
    lv_obj_center(g_lcd_ui.orb_glow);
    lv_obj_set_style_radius(g_lcd_ui.orb_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_lcd_ui.orb_glow, lv_color_hex(0x67E8F9), 0);
    lv_obj_set_style_bg_opa(g_lcd_ui.orb_glow, LV_OPA_20, 0);

    g_lcd_ui.orb_ring = lv_obj_create(orb_stage);
    lv_obj_remove_style_all(g_lcd_ui.orb_ring);
    lv_obj_set_size(g_lcd_ui.orb_ring, LCD_LVGL_ORB_RING_BASE, LCD_LVGL_ORB_RING_BASE);
    lv_obj_center(g_lcd_ui.orb_ring);
    lv_obj_set_style_radius(g_lcd_ui.orb_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g_lcd_ui.orb_ring, 2, 0);
    lv_obj_set_style_border_color(g_lcd_ui.orb_ring, lv_color_hex(0xA8E1FF), 0);
    lv_obj_set_style_border_opa(g_lcd_ui.orb_ring, LV_OPA_40, 0);
    lv_obj_set_style_bg_opa(g_lcd_ui.orb_ring, LV_OPA_TRANSP, 0);

    g_lcd_ui.orb_core = lv_obj_create(orb_stage);
    lv_obj_set_size(g_lcd_ui.orb_core, LCD_LVGL_ORB_CORE_BASE, LCD_LVGL_ORB_CORE_BASE);
    lv_obj_center(g_lcd_ui.orb_core);
    lv_obj_set_style_radius(g_lcd_ui.orb_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g_lcd_ui.orb_core, 0, 0);
    lv_obj_set_style_bg_color(g_lcd_ui.orb_core, lv_color_hex(0x67E8F9), 0);
    lv_obj_set_style_bg_grad_color(g_lcd_ui.orb_core, lv_color_hex(0xC4F8FF), 0);
    lv_obj_set_style_bg_grad_dir(g_lcd_ui.orb_core, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(g_lcd_ui.orb_core, 18, 0);
    lv_obj_set_style_shadow_opa(g_lcd_ui.orb_core, LV_OPA_30, 0);
    lv_obj_clear_flag(g_lcd_ui.orb_core, LV_OBJ_FLAG_SCROLLABLE);

    g_lcd_ui.phase_label = lv_label_create(g_lcd_ui.orb_core);
    lv_label_set_text(g_lcd_ui.phase_label, "就绪");
    lv_obj_set_style_text_font(g_lcd_ui.phase_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.phase_label, lv_color_hex(0x061117), 0);
    lv_obj_center(g_lcd_ui.phase_label);

    wave_stage = lv_obj_create(g_lcd_ui.voice_view);
    lv_obj_remove_style_all(wave_stage);
    lv_obj_set_size(wave_stage, 112, 24);
    lv_obj_align_to(wave_stage, orb_stage, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    for (int i = 0; i < LCD_LVGL_WAVE_COUNT; i++) {
        g_lcd_ui.bars[i] = lv_obj_create(wave_stage);
        lv_obj_remove_style_all(g_lcd_ui.bars[i]);
        lv_obj_set_size(g_lcd_ui.bars[i], LCD_LVGL_BAR_WIDTH, 8);
        lv_obj_align(g_lcd_ui.bars[i], LV_ALIGN_BOTTOM_LEFT, 12 + (i * 34), 0);
        lv_obj_set_style_radius(g_lcd_ui.bars[i], 6, 0);
        lv_obj_set_style_bg_color(g_lcd_ui.bars[i], lv_color_hex(0x67E8F9), 0);
        lv_obj_set_style_bg_opa(g_lcd_ui.bars[i], LV_OPA_50, 0);
    }

    g_lcd_ui.status_label = lv_label_create(g_lcd_ui.voice_view);
    lv_obj_set_size(g_lcd_ui.status_label, 248, 34);
    lv_label_set_long_mode(g_lcd_ui.status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_lcd_ui.status_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_align(g_lcd_ui.status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_lcd_ui.status_label, lv_color_hex(0xA8B6C9), 0);
    lv_label_set_text(g_lcd_ui.status_label, "按住 KEY1 开始说话");
    lv_obj_align_to(g_lcd_ui.status_label, wave_stage, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    light_panel = lv_obj_create(g_lcd_ui.voice_view);
    lv_obj_set_size(light_panel, LCD_VOICE_PANEL_WIDTH, LCD_VOICE_LIGHT_HEIGHT);
    lv_obj_align_to(light_panel, g_lcd_ui.status_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lcd_lvgl_style_panel(light_panel);

    g_lcd_ui.light_title_label = lv_label_create(light_panel);
    lv_label_set_text(g_lcd_ui.light_title_label, "灯光状态");
    lv_obj_set_style_text_font(g_lcd_ui.light_title_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.light_title_label, lv_color_hex(0x8FA3C2), 0);
    lv_obj_align(g_lcd_ui.light_title_label, LV_ALIGN_TOP_LEFT, 14, 10);

    g_lcd_ui.light_text_label = lv_label_create(light_panel);
    lv_obj_set_width(g_lcd_ui.light_text_label, 190);
    lv_label_set_long_mode(g_lcd_ui.light_text_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(g_lcd_ui.light_text_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.light_text_label, lv_color_hex(0xE4ECF8), 0);
    lv_label_set_text(g_lcd_ui.light_text_label, "已关闭");
    lv_obj_align(g_lcd_ui.light_text_label, LV_ALIGN_TOP_LEFT, 88, 10);

    stt_panel = lv_obj_create(g_lcd_ui.voice_view);
    lv_obj_set_size(stt_panel, LCD_VOICE_PANEL_WIDTH, LCD_VOICE_STT_HEIGHT);
    lv_obj_align_to(stt_panel, light_panel, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lcd_lvgl_style_panel(stt_panel);

    g_lcd_ui.stt_title_label = lv_label_create(stt_panel);
    lv_label_set_text(g_lcd_ui.stt_title_label, "识别内容");
    lv_obj_set_style_text_font(g_lcd_ui.stt_title_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.stt_title_label, lv_color_hex(0x8FA3C2), 0);

    g_lcd_ui.stt_text_label = lv_label_create(stt_panel);
    lv_obj_set_width(g_lcd_ui.stt_text_label, 260);
    lv_label_set_long_mode(g_lcd_ui.stt_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_lcd_ui.stt_text_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.stt_text_label, lv_color_hex(0xE4ECF8), 0);
    lv_label_set_text(g_lcd_ui.stt_text_label, "按住 KEY1 开始说话");
    lcd_lvgl_center_panel_stack(stt_panel, g_lcd_ui.stt_title_label, g_lcd_ui.stt_text_label, 6);

    tts_panel = lv_obj_create(g_lcd_ui.voice_view);
    lv_obj_set_size(tts_panel, LCD_VOICE_PANEL_WIDTH, LCD_VOICE_TTS_HEIGHT);
    lv_obj_align_to(tts_panel, stt_panel, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lcd_lvgl_style_panel(tts_panel);

    g_lcd_ui.tts_title_label = lv_label_create(tts_panel);
    lv_label_set_text(g_lcd_ui.tts_title_label, "小智回复");
    lv_obj_set_style_text_font(g_lcd_ui.tts_title_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.tts_title_label, lv_color_hex(0x8FA3C2), 0);

    g_lcd_ui.tts_text_label = lv_label_create(tts_panel);
    lv_obj_set_width(g_lcd_ui.tts_text_label, 260);
    lv_label_set_long_mode(g_lcd_ui.tts_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_lcd_ui.tts_text_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.tts_text_label, lv_color_hex(0xE4ECF8), 0);
    lv_label_set_text(g_lcd_ui.tts_text_label, "小智的语音回复会显示在这里");
    lcd_lvgl_center_panel_stack(tts_panel, g_lcd_ui.tts_title_label, g_lcd_ui.tts_text_label, 8);

    g_lcd_ui.camera_title_label = lv_label_create(g_lcd_ui.camera_view);
    lv_label_set_text(g_lcd_ui.camera_title_label, "摄像头 / 人脸识别");
    lv_obj_set_style_text_font(g_lcd_ui.camera_title_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.camera_title_label, lv_color_hex(0xD9E2F0), 0);
    lv_obj_align(g_lcd_ui.camera_title_label, LV_ALIGN_TOP_LEFT, 18, 16);

    g_lcd_ui.camera_note_label = lv_label_create(g_lcd_ui.camera_view);
    lv_obj_set_width(g_lcd_ui.camera_note_label, 284);
    lv_label_set_long_mode(g_lcd_ui.camera_note_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_lcd_ui.camera_note_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.camera_note_label, lv_color_hex(0x8FA3C2), 0);
    lv_label_set_text(g_lcd_ui.camera_note_label, "人脸识别准备中");
    lv_obj_align(g_lcd_ui.camera_note_label, LV_ALIGN_TOP_LEFT, 18, 42);

    camera_info_panel = lv_obj_create(g_lcd_ui.camera_view);
    lv_obj_set_size(camera_info_panel, LCD_VOICE_PANEL_WIDTH, LCD_CAMERA_INFO_HEIGHT);
    lv_obj_align(camera_info_panel, LV_ALIGN_BOTTOM_MID, 0, -18);
    lcd_lvgl_style_panel(camera_info_panel);

    camera_info_title = lv_label_create(camera_info_panel);
    lv_label_set_text(camera_info_title, "操作提示");
    lv_obj_set_style_text_font(camera_info_title, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(camera_info_title, lv_color_hex(0x8FA3C2), 0);
    lv_obj_align(camera_info_title, LV_ALIGN_TOP_LEFT, 14, 10);

    g_lcd_ui.camera_hint_label = lv_label_create(camera_info_panel);
    lv_obj_set_size(g_lcd_ui.camera_hint_label, 248, 40);
    lv_label_set_long_mode(g_lcd_ui.camera_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_lcd_ui.camera_hint_label, LCD_LVGL_FONT_MAIN, 0);
    lv_obj_set_style_text_color(g_lcd_ui.camera_hint_label, lv_color_hex(0xE4ECF8), 0);
    lv_label_set_text(g_lcd_ui.camera_hint_label, "KEY1 短按录入 / 长按清库\nKEY2 切回小智");
    lv_obj_align(g_lcd_ui.camera_hint_label, LV_ALIGN_TOP_LEFT, 14, 34);

    lcd_lvgl_apply_view(g_lcd_runtime.active_view);
}

static void lcd_lvgl_refresh_visuals(const lcd_lvgl_theme_t *theme, const xiaozhi_ui_snapshot_t *snapshot)
{
    int phase_speed = 1;
    int phase_amp = 4;
    int min_height = 6;
    int bar_amp = 6;
    int wave;
    int glow_size;
    int ring_size;
    int core_size;
    bool animated = lcd_lvgl_phase_is_animated(snapshot->phase);

    switch (snapshot->phase) {
        case XIAOZHI_UI_PHASE_LISTENING:
            phase_speed = 3;
            phase_amp = 8;
            min_height = 8;
            bar_amp = 10;
            break;

        case XIAOZHI_UI_PHASE_THINKING:
            phase_speed = 2;
            phase_amp = 6;
            min_height = 7;
            bar_amp = 8;
            break;

        case XIAOZHI_UI_PHASE_SPEAKING:
            phase_speed = 3;
            phase_amp = 6;
            min_height = 8;
            bar_amp = 9;
            break;

        default:
            phase_speed = 1;
            phase_amp = 2;
            min_height = 5;
            bar_amp = 4;
            break;
    }

    g_lcd_ui.anim_tick += phase_speed;
    wave = lcd_lvgl_triangle_wave(g_lcd_ui.anim_tick, 40);

    glow_size = LCD_LVGL_ORB_GLOW_BASE + ((wave * phase_amp) / 12);
    ring_size = LCD_LVGL_ORB_RING_BASE + ((wave * phase_amp) / 16);
    core_size = LCD_LVGL_ORB_CORE_BASE + ((wave * phase_amp) / 20);

    lv_obj_set_size(g_lcd_ui.orb_glow, glow_size, glow_size);
    lv_obj_center(g_lcd_ui.orb_glow);
    lv_obj_set_style_bg_color(g_lcd_ui.orb_glow, theme->accent, 0);
    lv_obj_set_style_bg_opa(g_lcd_ui.orb_glow, animated ? LV_OPA_20 : LV_OPA_10, 0);

    lv_obj_set_size(g_lcd_ui.orb_ring, ring_size, ring_size);
    lv_obj_center(g_lcd_ui.orb_ring);
    lv_obj_set_style_border_color(g_lcd_ui.orb_ring, theme->accent_soft, 0);

    lv_obj_set_size(g_lcd_ui.orb_core, core_size, core_size);
    lv_obj_center(g_lcd_ui.orb_core);
    lv_obj_set_style_bg_color(g_lcd_ui.orb_core, theme->accent, 0);
    lv_obj_set_style_bg_grad_color(g_lcd_ui.orb_core, theme->accent_soft, 0);
    lv_obj_set_style_shadow_color(g_lcd_ui.orb_core, theme->accent, 0);
    lv_obj_set_style_shadow_width(g_lcd_ui.orb_core, animated ? 22 : 14, 0);

    for (int i = 0; i < LCD_LVGL_WAVE_COUNT; i++) {
        int bar_wave = lcd_lvgl_triangle_wave(g_lcd_ui.anim_tick + (i * 6), 28);
        int bar_height = min_height + ((bar_wave * bar_amp) / 18);

        lv_obj_set_size(g_lcd_ui.bars[i], LCD_LVGL_BAR_WIDTH, bar_height);
        lv_obj_align(g_lcd_ui.bars[i], LV_ALIGN_BOTTOM_LEFT, 12 + (i * 34), 0);
        lv_obj_set_style_bg_color(g_lcd_ui.bars[i], lv_color_mix(theme->accent_soft, theme->accent, (uint8_t)(48 + i * 36)), 0);
        lv_obj_set_style_bg_opa(g_lcd_ui.bars[i], animated ? LV_OPA_70 : LV_OPA_30, 0);
    }
}

static void lcd_lvgl_update_ui(lv_timer_t *timer)
{
    xiaozhi_ui_snapshot_t snapshot;
    lcd_lvgl_theme_t theme;
    bool state_changed;

    LV_UNUSED(timer);

    xiaozhi_ui_status_get_snapshot(&snapshot);
    theme = lcd_lvgl_get_theme(snapshot.phase);
    state_changed = (!g_lcd_ui.snapshot_valid) ||
                    (snapshot.phase != g_lcd_ui.last_snapshot.phase) ||
                    (strcmp(snapshot.note, g_lcd_ui.last_snapshot.note) != 0) ||
                    (strcmp(snapshot.wifi_ssid, g_lcd_ui.last_snapshot.wifi_ssid) != 0) ||
                    (strcmp(snapshot.wifi_ip, g_lcd_ui.last_snapshot.wifi_ip) != 0) ||
                    (strcmp(snapshot.binding_text, g_lcd_ui.last_snapshot.binding_text) != 0) ||
                    (strcmp(snapshot.stt_text, g_lcd_ui.last_snapshot.stt_text) != 0) ||
                    (strcmp(snapshot.light_text, g_lcd_ui.last_snapshot.light_text) != 0) ||
                    (strcmp(snapshot.tts_text, g_lcd_ui.last_snapshot.tts_text) != 0);

    if (state_changed) {
        lv_label_set_text(g_lcd_ui.setup_status_label,
                          (snapshot.note[0] != '\0') ? snapshot.note : "正在准备联网和绑定信息");
        lv_label_set_text(g_lcd_ui.setup_wifi_text_label,
                          (snapshot.wifi_ssid[0] != '\0') ? snapshot.wifi_ssid : "未连接");
        lv_label_set_text(g_lcd_ui.setup_ip_text_label,
                          (snapshot.wifi_ip[0] != '\0') ? snapshot.wifi_ip : "--");
        lv_label_set_text(g_lcd_ui.setup_binding_text_label,
                          (snapshot.binding_text[0] != '\0') ? snapshot.binding_text : "查询中");
        lv_label_set_text(g_lcd_ui.phase_label, theme.phase_text);
        lv_label_set_text(g_lcd_ui.status_label,
                          (snapshot.note[0] != '\0') ? snapshot.note : theme.status_text);
        lv_label_set_text(g_lcd_ui.light_text_label,
                          (snapshot.light_text[0] != '\0') ? snapshot.light_text : "已关闭");
        lv_label_set_text(g_lcd_ui.stt_text_label,
                          (snapshot.stt_text[0] != '\0') ? snapshot.stt_text : "按住 KEY1 开始说话");
        lv_label_set_text(g_lcd_ui.tts_text_label,
                          (snapshot.tts_text[0] != '\0') ? snapshot.tts_text : "小智的语音回复会显示在这里");
        lcd_lvgl_center_panel_stack(lv_obj_get_parent(g_lcd_ui.stt_title_label),
                                    g_lcd_ui.stt_title_label,
                                    g_lcd_ui.stt_text_label,
                                    6);
        lcd_lvgl_center_panel_stack(lv_obj_get_parent(g_lcd_ui.tts_title_label),
                                    g_lcd_ui.tts_title_label,
                                    g_lcd_ui.tts_text_label,
                                    8);
    }

    if (state_changed || lcd_lvgl_phase_is_animated(snapshot.phase)) {
        lcd_lvgl_refresh_visuals(&theme, &snapshot);
    }

    g_lcd_ui.last_snapshot = snapshot;
    g_lcd_ui.snapshot_valid = true;
}

static void lcd_lvgl_task(void *pvParameters)
{
    lv_disp_t *disp;
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lcd_lvgl_tick_cb,
        .name = "lvgl_tick",
    };

    (void)pvParameters;

    if (lcd_lvgl_reserve_buffer() != ESP_OK) {
        ESP_LOGE(TAG, "lvgl draw buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    if (lcd_io_done_sem == NULL) {
        lcd_io_done_sem = xSemaphoreCreateBinary();
    }
    if (lcd_io_done_sem == NULL) {
        ESP_LOGE(TAG, "lcd io semaphore alloc failed");
        vTaskDelete(NULL);
        return;
    }

    lv_init();
    if (!lcd_st7796_is_initialized()) {
        ESP_LOGE(TAG, "lcd not initialized, call lcd_st7796_init() in main.c first");
        vTaskDelete(NULL);
        return;
    }
    ESP_ERROR_CHECK(lcd_st7796_register_event_callbacks(&(esp_lcd_panel_io_callbacks_t) {
        .on_color_trans_done = lcd_lvgl_flush_ready_cb,
    }, &lcd_disp_drv));

    lv_disp_draw_buf_init(&lcd_draw_buf, lcd_buf_1, NULL, LCD_ST7796_H_RES * LCD_LVGL_BUF_LINES);

    lv_disp_drv_init(&lcd_disp_drv);
    lcd_disp_drv.hor_res = LCD_ST7796_H_RES;
    lcd_disp_drv.ver_res = LCD_ST7796_V_RES;
    lcd_disp_drv.flush_cb = lcd_lvgl_flush_cb;
    lcd_disp_drv.draw_buf = &lcd_draw_buf;
    lcd_disp_drv.full_refresh = 0;
    disp = lv_disp_drv_register(&lcd_disp_drv);
    lv_disp_set_default(disp);

    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &lcd_lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lcd_lvgl_tick_timer, LCD_LVGL_TICK_MS * 1000));

    vTaskDelay(pdMS_TO_TICKS(LCD_LVGL_BOOT_DELAY_MS));
    lcd_lvgl_build_ui();
    lcd_lvgl_switch_key_init();
    lcd_lvgl_camera_init_runtime();
    lcd_lvgl_camera_set_bindings(&(lcd_lvgl_camera_bindings_t) {
        .note_label = g_lcd_ui.camera_note_label,
        .io_done_sem = lcd_io_done_sem,
        .fallback_buf = lcd_buf_1,
        .fallback_buf_lines = LCD_LVGL_BUF_LINES,
    });
    lv_timer_create(lcd_lvgl_update_ui, LCD_LVGL_UPDATE_MS, NULL);
    ESP_LOGI(TAG, "lvgl chinese ui start");

    while (1) {
        xiaozhi_ui_view_t target_view;

        lcd_lvgl_camera_service_beep();
        lcd_lvgl_poll_switch_key();
        lcd_lvgl_camera_poll_key(g_lcd_runtime.active_view == XIAOZHI_UI_VIEW_CAMERA);
        target_view = xiaozhi_ui_status_get_view();

        if (target_view != g_lcd_runtime.active_view) {
            if (target_view == XIAOZHI_UI_VIEW_CAMERA) {
                esp_err_t ret = lcd_lvgl_camera_enter();

                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(ret));
                    lcd_lvgl_camera_leave();
                    xiaozhi_leave_camera_view();
                    xiaozhi_ui_status_set_view(XIAOZHI_UI_VIEW_VOICE);
                    xiaozhi_ui_status_set_phase(XIAOZHI_UI_PHASE_ERROR);
                    xiaozhi_ui_status_set_note("摄像头初始化失败");
                    g_lcd_runtime.active_view = XIAOZHI_UI_VIEW_VOICE;
                    lcd_lvgl_apply_view(g_lcd_runtime.active_view);
                    g_lcd_ui.snapshot_valid = false;
                    lv_timer_handler();
                    vTaskDelay(pdMS_TO_TICKS(LCD_LVGL_SWITCH_KEY_MS));
                    continue;
                }
            } else {
                lcd_lvgl_camera_leave();
                g_lcd_ui.snapshot_valid = false;
                lv_obj_invalidate(lv_scr_act());
            }

            g_lcd_runtime.active_view = target_view;
            lcd_lvgl_apply_view(target_view);
            if (target_view != XIAOZHI_UI_VIEW_CAMERA) {
                xiaozhi_leave_camera_view();
            }
            if (target_view != XIAOZHI_UI_VIEW_CAMERA) {
                lv_timer_handler();
            }
        }

        if (g_lcd_runtime.active_view == XIAOZHI_UI_VIEW_CAMERA) {
            if (lcd_lvgl_camera_take_overlay_pending()) {
                lv_timer_handler();
            }
            lcd_lvgl_camera_render_frame();
            vTaskDelay(pdMS_TO_TICKS(LCD_CAMERA_FRAME_DELAY_MS));
        } else {
            uint32_t wait_ms = lv_timer_handler();

            if (wait_ms < LCD_LVGL_HANDLER_MIN_MS) {
                wait_ms = LCD_LVGL_HANDLER_MIN_MS;
            } else if (wait_ms > LCD_LVGL_HANDLER_MAX_MS) {
                wait_ms = LCD_LVGL_HANDLER_MAX_MS;
            }

            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        }
    }
}

BaseType_t lcd_lvgl_task_create(void)
{
    return xTaskCreate(lcd_lvgl_task, "lcd_lvgl_task", LCD_LVGL_TASK_STACK, NULL, 6, NULL);
}

esp_err_t lcd_lvgl_reserve_buffer(void)
{
    if (lcd_buf_1 != NULL) {
        lcd_lvgl_camera_reserve_buffers();
        return ESP_OK;
    }

    lcd_buf_1 = heap_caps_malloc(LCD_ST7796_H_RES * LCD_LVGL_BUF_LINES * sizeof(lv_color_t),
                                 MALLOC_CAP_DMA);
    if (lcd_buf_1 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lcd_lvgl_camera_reserve_buffers();
    return ESP_OK;
}
