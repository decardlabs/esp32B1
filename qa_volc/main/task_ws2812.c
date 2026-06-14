#include "task_ws2812.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "freertos/queue.h"
// #include "xiaozhi_ui_status.h"


static const char *TAG = "TASK_WS2812";

typedef enum {
    WS2812_MODE_OFF = 0,
    WS2812_MODE_SOLID,
    WS2812_MODE_BREATHING,
    WS2812_MODE_COMET,
    WS2812_MODE_RAINBOW,
} ws2812_mode_t;

typedef struct {
    bool mode_valid;
    ws2812_mode_t mode;
    bool color_valid;
    bool brightness_set_valid;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t brightness;
    unsigned brightness_percent;
    int brightness_delta;
} ws2812_command_t;

typedef struct {
    ws2812_mode_t mode;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t brightness;
    uint16_t breathing_phase;
    uint16_t rainbow_hue;
    int comet_head;
    int comet_dir;
} ws2812_runtime_t;

#define WS2812_TASK_STACK          6144
#define WS2812_TASK_PRIO           10
#define WS2812_QUEUE_LEN           8
#define WS2812_FRAME_MS            20
#define WS2812_BRIGHTNESS_DEFAULT  96
#define WS2812_BRIGHTNESS_STEP     32
#define WS2812_BRIGHTNESS_MIN      16
#define WS2812_BRIGHTNESS_MAX      255

static QueueHandle_t s_ws2812_queue = NULL;
static TaskHandle_t s_ws2812_task_handle = NULL;
static ws2812_runtime_t s_ws2812_state = {
    .mode = WS2812_MODE_OFF,
    .r = 255,
    .g = 255,
    .b = 255,
    .brightness = WS2812_BRIGHTNESS_DEFAULT,
    .comet_dir = 1,
};

static const char *ws2812_mode_name(ws2812_mode_t mode)
{
    switch (mode) {
        case WS2812_MODE_OFF:
            return "已关闭";
        case WS2812_MODE_SOLID:
            return "常亮";
        case WS2812_MODE_BREATHING:
            return "呼吸";
        case WS2812_MODE_COMET:
            return "流水";
        case WS2812_MODE_RAINBOW:
            return "彩虹";
        default:
            return "未知";
    }
}

static const char *ws2812_color_name(uint8_t r, uint8_t g, uint8_t b)
{
    if ((r == 255) && (g == 0) && (b == 0)) {
        return "红色";
    }
    if ((r == 0) && (g == 255) && (b == 0)) {
        return "绿色";
    }
    if ((r == 0) && (g == 80) && (b == 255)) {
        return "蓝色";
    }
    if ((r == 255) && (g == 255) && (b == 255)) {
        return "白色";
    }
    if ((r == 255) && (g == 180) && (b == 0)) {
        return "黄色";
    }
    if ((r == 180) && (g == 0) && (b == 255)) {
        return "紫色";
    }
    if ((r == 0) && (g == 200) && (b == 200)) {
        return "青色";
    }

    return "自定义";
}

void ws2812_get_state_text(char *text, size_t text_len)
{
    unsigned brightness_percent;
    const char *mode_name;
    const char *color_name;

    if ((text == NULL) || (text_len == 0U)) {
        return;
    }

    mode_name = ws2812_mode_name(s_ws2812_state.mode);
    brightness_percent = ((unsigned)s_ws2812_state.brightness * 100U) / 255U;
    color_name = ws2812_color_name(s_ws2812_state.r, s_ws2812_state.g, s_ws2812_state.b);

    if (s_ws2812_state.mode == WS2812_MODE_OFF) {
        snprintf(text, text_len, "%s", mode_name);
    } else if (s_ws2812_state.mode == WS2812_MODE_RAINBOW) {
        snprintf(text, text_len, "%s %u%%", mode_name, brightness_percent);
    } else {
        snprintf(text, text_len, "%s %s %u%%", mode_name, color_name, brightness_percent);
    }
}

static void ws2812_publish_state(void)
{
    char state_text[64];

    ws2812_get_state_text(state_text, sizeof(state_text));
    // xiaozhi_ui_status_set_light_text(state_text);
}

static bool ws2812_text_contains(const char *text, const char *needle)
{
    return (text != NULL) && (needle != NULL) && (strstr(text, needle) != NULL);
}

static bool ws2812_parse_uint_percent(const char *text, unsigned *value_out)
{
    const char *p = text;
    unsigned value = 0;
    bool has_digit = false;

    if ((text == NULL) || (value_out == NULL)) {
        return false;
    }

    while ((*p != '\0') && !isdigit((unsigned char)*p)) {
        p++;
    }

    while (isdigit((unsigned char)*p)) {
        has_digit = true;
        value = (value * 10U) + (unsigned)(*p - '0');
        p++;
    }

    if (!has_digit) {
        return false;
    }

    if (value > 100U) {
        value = 100U;
    }

    *value_out = value;
    return true;
}

static bool ws2812_parse_brightness_percent(const char *text, uint8_t *brightness, unsigned *percent_out)
{
    const char *brightness_pos;
    const char *percent_pos;
    unsigned percent = 0;

    if ((text == NULL) || (brightness == NULL) || (percent_out == NULL)) {
        return false;
    }

    brightness_pos = strstr(text, "亮度");
    if ((brightness_pos != NULL) && ws2812_parse_uint_percent(brightness_pos + strlen("亮度"), &percent)) {
        *percent_out = percent;
        *brightness = (uint8_t)((percent * 255U + 50U) / 100U);
        return true;
    }

    percent_pos = strstr(text, "百分之");
    if ((percent_pos != NULL) && ws2812_parse_uint_percent(percent_pos + strlen("百分之"), &percent)) {
        *percent_out = percent;
        *brightness = (uint8_t)((percent * 255U + 50U) / 100U);
        return true;
    }

    return false;
}

static uint8_t ws2812_clamp_brightness(int brightness)
{
    if (brightness < WS2812_BRIGHTNESS_MIN) {
        return WS2812_BRIGHTNESS_MIN;
    }
    if (brightness > WS2812_BRIGHTNESS_MAX) {
        return WS2812_BRIGHTNESS_MAX;
    }

    return (uint8_t)brightness;
}

static void ws2812_reset_animation(ws2812_runtime_t *state)
{
    state->breathing_phase = 0;
    state->rainbow_hue = 0;
    state->comet_head = 0;
    state->comet_dir = 1;
}

static void ws2812_apply_command(ws2812_runtime_t *state, const ws2812_command_t *cmd)
{
    if ((state == NULL) || (cmd == NULL)) {
        return;
    }

    if (cmd->brightness_delta != 0) {
        state->brightness = ws2812_clamp_brightness((int)state->brightness + cmd->brightness_delta);
    }
    if (cmd->brightness_set_valid) {
        state->brightness = cmd->brightness;
    }

    if (cmd->color_valid) {
        state->r = cmd->r;
        state->g = cmd->g;
        state->b = cmd->b;
    }

    if (cmd->mode_valid) {
        if (state->mode != cmd->mode) {
            ws2812_reset_animation(state);
        }
        state->mode = cmd->mode;
    }
}

static esp_err_t ws2812_render(const ws2812_runtime_t *state)
{
    switch (state->mode) {
        case WS2812_MODE_OFF:
            return ws2812_clear_all();

        case WS2812_MODE_SOLID:
            return ws2812_show_color(state->r, state->g, state->b, state->brightness);

        case WS2812_MODE_BREATHING:
            return ws2812_show_breathing_step(state->r, state->g, state->b, state->brightness, state->breathing_phase);

        case WS2812_MODE_COMET:
            return ws2812_show_comet_step(state->brightness, state->comet_head);

        case WS2812_MODE_RAINBOW:
            return ws2812_show_rainbow_step(state->brightness, state->rainbow_hue);

        default:
            return ESP_ERR_INVALID_STATE;
    }
}

static void ws2812_advance_animation(ws2812_runtime_t *state)
{
    switch (state->mode) {
        case WS2812_MODE_BREATHING:
            state->breathing_phase = (uint16_t)((state->breathing_phase + 1U) % 200U);
            break;

        case WS2812_MODE_RAINBOW:
            state->rainbow_hue = (uint16_t)((state->rainbow_hue + 2U) % 360U);
            break;

        case WS2812_MODE_COMET:
            state->comet_head += state->comet_dir;
            if (state->comet_head >= (LED_STRIP_LED_COUNT - 1)) {
                state->comet_head = LED_STRIP_LED_COUNT - 1;
                state->comet_dir = -1;
            } else if (state->comet_head <= 0) {
                state->comet_head = 0;
                state->comet_dir = 1;
            }
            break;

        default:
            break;
    }
}

static bool ws2812_parse_color(const char *text, uint8_t *r, uint8_t *g, uint8_t *b, const char **name)
{
    if (ws2812_text_contains(text, "红")) {
        *r = 255; *g = 0; *b = 0; *name = "红色";
        return true;
    }
    if (ws2812_text_contains(text, "绿")) {
        *r = 0; *g = 255; *b = 0; *name = "绿色";
        return true;
    }
    if (ws2812_text_contains(text, "蓝")) {
        *r = 0; *g = 80; *b = 255; *name = "蓝色";
        return true;
    }
    if (ws2812_text_contains(text, "白")) {
        *r = 255; *g = 255; *b = 255; *name = "白色";
        return true;
    }
    if (ws2812_text_contains(text, "黄")) {
        *r = 255; *g = 180; *b = 0; *name = "黄色";
        return true;
    }
    if (ws2812_text_contains(text, "紫")) {
        *r = 180; *g = 0; *b = 255; *name = "紫色";
        return true;
    }
    if (ws2812_text_contains(text, "青")) {
        *r = 0; *g = 200; *b = 200; *name = "青色";
        return true;
    }

    return false;
}

static void ws2812_set_feedback(char *feedback, size_t feedback_len, const char *format, const char *arg)
{
    if ((feedback == NULL) || (feedback_len == 0U)) {
        return;
    }

    if (arg != NULL) {
        snprintf(feedback, feedback_len, format, arg);
    } else {
        snprintf(feedback, feedback_len, "%s", format);
    }
}

static void ws2812_queue_command(const ws2812_command_t *cmd)
{
    ws2812_command_t dropped_cmd;

    if ((cmd == NULL) || (s_ws2812_queue == NULL)) {
        return;
    }

    if (xQueueSend(s_ws2812_queue, cmd, 0) != pdPASS) {
        xQueueReceive(s_ws2812_queue, &dropped_cmd, 0);
        xQueueSend(s_ws2812_queue, cmd, 0);
    }
}

bool ws2812_handle_voice_command(const char *text, char *feedback, size_t feedback_len)
{
    ws2812_command_t cmd = {0};
    char brightness_suffix[24] = {0};
    const char *color_name = NULL;
    bool has_color = false;
    bool power_off;
    bool power_on;
    bool breathing;
    bool rainbow;
    bool comet;
    bool brighter;
    bool darker;
    bool exact_brightness;

    if ((text == NULL) || (text[0] == '\0')) {
        return false;
    }
    if (s_ws2812_queue == NULL) {
        ws2812_set_feedback(feedback, feedback_len, "灯带任务未启动", NULL);
        return false;
    }

    power_off = ws2812_text_contains(text, "关灯")
        || ws2812_text_contains(text, "熄灯")
        || ws2812_text_contains(text, "灭灯");
    power_on = ws2812_text_contains(text, "开灯")
        || ws2812_text_contains(text, "亮灯")
        || ws2812_text_contains(text, "点灯");
    breathing = ws2812_text_contains(text, "呼吸");
    rainbow = ws2812_text_contains(text, "彩虹");
    comet = ws2812_text_contains(text, "流水")
        || ws2812_text_contains(text, "跑马")
        || ws2812_text_contains(text, "流光")
        || ws2812_text_contains(text, "彗星");
    brighter = ws2812_text_contains(text, "调亮")
        || ws2812_text_contains(text, "亮一点")
        || ws2812_text_contains(text, "亮一些")
        || ws2812_text_contains(text, "更亮");
    darker = ws2812_text_contains(text, "调暗")
        || ws2812_text_contains(text, "暗一点")
        || ws2812_text_contains(text, "暗一些")
        || ws2812_text_contains(text, "更暗");
    exact_brightness = ws2812_parse_brightness_percent(text, &cmd.brightness, &cmd.brightness_percent);
    cmd.brightness_set_valid = exact_brightness;

    has_color = ws2812_parse_color(text, &cmd.r, &cmd.g, &cmd.b, &color_name);
    cmd.color_valid = has_color;

    if (power_off) {
        cmd.mode_valid = true;
        cmd.mode = WS2812_MODE_OFF;
        ws2812_queue_command(&cmd);
        ws2812_set_feedback(feedback, feedback_len, "已关灯", NULL);
        return true;
    }

    if (brighter) {
        cmd.brightness_delta += WS2812_BRIGHTNESS_STEP;
    }
    if (darker) {
        cmd.brightness_delta -= WS2812_BRIGHTNESS_STEP;
    }

    if (rainbow) {
        cmd.mode_valid = true;
        cmd.mode = WS2812_MODE_RAINBOW;
    } else if (comet) {
        cmd.mode_valid = true;
        cmd.mode = WS2812_MODE_COMET;
    } else if (breathing) {
        cmd.mode_valid = true;
        cmd.mode = WS2812_MODE_BREATHING;
    } else if (power_on || has_color) {
        cmd.mode_valid = true;
        cmd.mode = WS2812_MODE_SOLID;
    } else if (exact_brightness && (s_ws2812_state.mode == WS2812_MODE_OFF)) {
        cmd.mode_valid = true;
        cmd.mode = WS2812_MODE_SOLID;
    }

    if (!cmd.mode_valid && !cmd.color_valid && !cmd.brightness_set_valid && (cmd.brightness_delta == 0)) {
        return false;
    }

    ws2812_queue_command(&cmd);

    if (exact_brightness) {
        snprintf(brightness_suffix, sizeof(brightness_suffix), " 亮度%u%%", cmd.brightness_percent);
    }

    if (rainbow) {
        char msg[48];
        snprintf(msg, sizeof(msg), "已切换彩虹灯%s", brightness_suffix);
        ws2812_set_feedback(feedback, feedback_len, msg, NULL);
    } else if (comet) {
        char msg[48];
        snprintf(msg, sizeof(msg), "已切换流水灯%s", brightness_suffix);
        ws2812_set_feedback(feedback, feedback_len, msg, NULL);
    } else if (breathing && has_color) {
        char msg[48];
        snprintf(msg, sizeof(msg), "已切换%s呼吸灯%s", color_name, brightness_suffix);
        ws2812_set_feedback(feedback, feedback_len, msg, NULL);
    } else if (breathing) {
        char msg[48];
        snprintf(msg, sizeof(msg), "已切换呼吸灯%s", brightness_suffix);
        ws2812_set_feedback(feedback, feedback_len, msg, NULL);
    } else if (has_color) {
        char msg[48];
        snprintf(msg, sizeof(msg), "已切换%s灯%s", color_name, brightness_suffix);
        ws2812_set_feedback(feedback, feedback_len, msg, NULL);
    } else if (exact_brightness) {
        char brightness_feedback[48];

        snprintf(brightness_feedback, sizeof(brightness_feedback), "已设置亮度%u%%", cmd.brightness_percent);
        ws2812_set_feedback(feedback, feedback_len, brightness_feedback, NULL);
    } else if (brighter) {
        ws2812_set_feedback(feedback, feedback_len, "已调亮灯光", NULL);
    } else if (darker) {
        ws2812_set_feedback(feedback, feedback_len, "已调暗灯光", NULL);
    } else if (power_on) {
        ws2812_set_feedback(feedback, feedback_len, "已开灯", NULL);
    } else {
        ws2812_set_feedback(feedback, feedback_len, "已执行灯光指令", NULL);
    }

    return true;
}

void ws2812_task(void *pvParameters)
{
    ws2812_command_t cmd;

    (void)pvParameters;
    if (!ws2812_is_initialized()) {
        ESP_LOGE(TAG, "ws2812 not initialized, call ws2812_init() in main.c first");
        vTaskDelete(NULL);
        return;
    }

    ESP_ERROR_CHECK(ws2812_clear_all());
    ws2812_publish_state();
    ESP_LOGI(TAG, "ws2812 control task started");

    while (1) {
        if (xQueueReceive(s_ws2812_queue, &cmd, pdMS_TO_TICKS(WS2812_FRAME_MS)) == pdPASS) {
            do {
                ws2812_apply_command(&s_ws2812_state, &cmd);
            } while (xQueueReceive(s_ws2812_queue, &cmd, 0) == pdPASS);
            ws2812_publish_state();
        }

        ESP_ERROR_CHECK(ws2812_render(&s_ws2812_state));
        ws2812_advance_animation(&s_ws2812_state);
    }
}
    


BaseType_t ws2812_task_create(void)
{
    if (s_ws2812_task_handle != NULL) {
        return pdPASS;
    }

    if (s_ws2812_queue == NULL) {
        s_ws2812_queue = xQueueCreate(WS2812_QUEUE_LEN, sizeof(ws2812_command_t));
        if (s_ws2812_queue == NULL) {
            ESP_LOGE(TAG, "create ws2812 queue failed");
            return pdFAIL;
        }
    }

    return xTaskCreate(ws2812_task, "ws2812_task", WS2812_TASK_STACK, NULL, WS2812_TASK_PRIO, &s_ws2812_task_handle);
}

