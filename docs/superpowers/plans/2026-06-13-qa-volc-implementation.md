# 火山引擎智能问答 MVP — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 在 ESP32-S3 上实现一个按键触发的语音问答系统，TTS→ASR→LLM→屏幕显示全链路。

**Architecture:** 基于 xiaozhi 项目的 BSP/device 驱动层复用，新增业务层模块。系统通过 config_parser 读取 TF 卡上的 config.ini 获取 Wi-Fi 和 API Key 配置，状态机驱动录音→ASR→LLM→UI 更新循环。

**Tech Stack:** ESP-IDF 5.x, LVGL 8.x, FreeRTOS, ESP-HTTP-Client, Volcengine ASR API, Volcengine Responses API

**Design Doc:** `docs/superpowers/specs/2026-06-13-qa-volc-design.md`

---

## 文件结构

```
qa_volc/
├── CMakeLists.txt                    # 顶层 CMake（复用 xiaozhi 模板）
├── partitions.csv                     # 复用 xiaozhi
├── sdkconfig / sdkconfig.defaults     # 从 xiaozhi 复制 + 调整
├── components/
│   ├── bsp/                           # 从 xiaozhi 完整复制
│   │   ├── bsp_audio.c, bsp_exti.c, bsp_gpio.c, bsp_gptimer.c
│   │   ├── bsp_i2c.c, bsp_ledc.c, bsp_spi.c, bsp_uart.c
│   │   └── include/
│   └── device/                        # 从 xiaozhi 完整复制
│       ├── es8388.c, lcd_st7796.c, tf_sdcard.c
│       ├── ws2812b.c, xl9555.c
│       └── include/
├── managed_components/               # 从 xiaozhi 复制（lvgl, lvgl_esp32_drivers 等）
├── main/
│   ├── CMakeLists.txt                # 源文件列表
│   ├── idf_component.yml             # 依赖声明
│   ├── main.c                        # 状态机 + 初始化
│   ├── config_parser.c / .h          # TF 卡 config.ini 解析
│   ├── app_wifi.c / .h               # Wi-Fi STA 连接
│   ├── task_audio_capture.c / .h     # KEY3 触发 I2S 录音
│   ├── task_volc_asr.c / .h          # 火山引擎 ASR HTTP 客户端
│   ├── task_volc_llm.c / .h          # 火山引擎 LLM SSE 客户端
│   ├── task_qa_lvgl.c / .h           # LVGL Q&A 界面
│   ├── task_sdcard.c / .h            # 复用 xiaozhi（对话日志）
│   ├── task_ws2812.c / .h            # 复用 xiaozhi（LED 状态）
│   └── task_timer.c / .h             # 复用 xiaozhi（定时器）
└── tools/
    ├── config.ini                    # 已验证通过的配置文件
    ├── verify_api.py                 # Python API 验证脚本（已完成）
    └── demo_qa_loop.py              # Python 全链路 Demo（已完成）
```

---

### Task 1: 项目脚手架 — 复制 BSP / Device / managed_components

**Files:**
- Create: `qa_volc/CMakeLists.txt`
- Create: `qa_volc/partitions.csv`
- Create: `qa_volc/sdkconfig`
- Create: `qa_volc/sdkconfig.defaults`
- Create: `qa_volc/main/CMakeLists.txt`
- Create: `qa_volc/main/idf_component.yml`
- Copy: `xiaozhi/components/bsp/` → `qa_volc/components/bsp/`
- Copy: `xiaozhi/components/device/` → `qa_volc/components/device/`
- Copy: `xiaozhi/managed_components/` → `qa_volc/managed_components/`
- Copy: `xiaozhi/main/lv_font_xiaozhi_cn_16.c` → `qa_volc/main/lv_font_xiaozhi_cn_16.c`

- [ ] **Step 1: 复制 BSP 和 device 驱动**

```bash
cd /Users/macairm5/Documents/esp32
cp -r xiaozhi/components/bsp qa_volc/components/bsp
cp -r xiaozhi/components/device qa_volc/components/device
cp -r xiaozhi/managed_components qa_volc/managed_components
cp xiaozhi/main/lv_font_xiaozhi_cn_16.c qa_volc/main/
cp xiaozhi/partitions.csv qa_volc/partitions.csv
cp xiaozhi/sdkconfig qa_volc/sdkconfig
cp xiaozhi/sdkconfig.defaults qa_volc/sdkconfig.defaults
```

- [ ] **Step 2: 创建顶层 CMakeLists.txt**

`qa_volc/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(qa_volc)
```

- [ ] **Step 3: 创建 main/CMakeLists.txt**

`qa_volc/main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS
        main.c
        config_parser.c
        app_wifi.c
        task_audio_capture.c
        task_volc_asr.c
        task_volc_llm.c
        task_qa_lvgl.c
        task_sdcard.c
        task_ws2812.c
        task_timer.c
        lv_font_xiaozhi_cn_16.c
    INCLUDE_DIRS
        include
        ../components/bsp/include
        ../components/device/include
    REQUIRES
        nvs_flash
        esp_wifi
        esp_http_client
        esp-tls
        mbedtls
        lvgl
        lvgl_esp32_drivers
)
```

- [ ] **Step 4: 创建 idf_component.yml**

`qa_volc/main/idf_component.yml`:
```yaml
dependencies:
  lvgl/lvgl: "^8.3.0"
  lvgl_esp32_drivers: "^1.0.0"
  idf: ">=5.0"
```

- [ ] **Step 5: 验证编译**

```bash
cd qa_volc
idf.py set-target esp32s3
idf.py build 2>&1 | tail -5
```
Expected: Build succeeds (with linker errors for missing .c files — expected since we haven't created them yet)

---

### Task 2: config_parser — TF 卡配置文件解析

**Files:**
- Create: `qa_volc/main/config_parser.h`
- Create: `qa_volc/main/config_parser.c`

- [ ] **Step 1: 创建头文件 config_parser.h**

`qa_volc/main/config_parser.h`:
```c
#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include "esp_err.h"

#define CONFIG_MAX_KEY_LEN   64
#define CONFIG_MAX_VAL_LEN   256
#define CONFIG_MAX_ENTRIES   32

typedef struct {
    char key[CONFIG_MAX_KEY_LEN];
    char val[CONFIG_MAX_VAL_LEN];
} config_entry_t;

typedef struct {
    config_entry_t entries[CONFIG_MAX_ENTRIES];
    int count;
} config_t;

/** 从文件路径解析 INI 格式配置 */
esp_err_t config_parse(const char *filepath, config_t *cfg);

/** 按 key 查找字符串值，未找到返回 default_val */
const char *config_get_string(const config_t *cfg, const char *key, const char *default_val);

/** 按 key 查找 int 值，未找到返回 default_val */
int config_get_int(const config_t *cfg, const char *key, int default_val);

#endif
```

- [ ] **Step 2: 实现 config_parser.c**

`qa_volc/main/config_parser.c`:
```c
#include "config_parser.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"

static const char *TAG = "CONFIG";

static void trim_whitespace(char *str) {
    char *start = str;
    char *end;
    while (isspace((unsigned char)*start)) start++;
    if (start != str) memmove(str, start, strlen(start) + 1);
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
}

static bool is_comment_or_empty(const char *line) {
    while (*line && isspace((unsigned char)*line)) line++;
    return (*line == '\0' || *line == '#' || *line == ';');
}

esp_err_t config_parse(const char *filepath, config_t *cfg) {
    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    cfg->count = 0;
    char line[CONFIG_MAX_KEY_LEN + CONFIG_MAX_VAL_LEN + 4];
    while (fgets(line, sizeof(line), f) && cfg->count < CONFIG_MAX_ENTRIES) {
        line[strcspn(line, "\r\n")] = '\0';
        if (is_comment_or_empty(line)) continue;

        char *eq = strchr(line, '=');
        if (eq == NULL) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_whitespace(key);
        trim_whitespace(val);
        if (strlen(key) == 0) continue;

        strncpy(cfg->entries[cfg->count].key, key, CONFIG_MAX_KEY_LEN - 1);
        strncpy(cfg->entries[cfg->count].val, val, CONFIG_MAX_VAL_LEN - 1);
        cfg->count++;
    }

    fclose(f);
    ESP_LOGI(TAG, "Parsed %d entries from %s", cfg->count, filepath);
    return ESP_OK;
}

const char *config_get_string(const config_t *cfg, const char *key, const char *default_val) {
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            return cfg->entries[i].val;
        }
    }
    return default_val;
}

int config_get_int(const config_t *cfg, const char *key, int default_val) {
    const char *val = config_get_string(cfg, key, NULL);
    if (val == NULL) return default_val;
    char *end;
    int result = (int)strtol(val, &end, 10);
    return (*end == '\0') ? result : default_val;
}
```

- [ ] **Step 3: 提交**

```bash
git add qa_volc/main/config_parser.c qa_volc/main/config_parser.h
git commit -m "feat(qa_volc): add config_parser for TF card config.ini"
```

---

### Task 3: app_wifi — Wi-Fi STA 连接

**Files:**
- Create: `qa_volc/main/app_wifi.h`
- Create: `qa_volc/main/app_wifi.c`

- [ ] **Step 1: 创建 app_wifi.h**

`qa_volc/main/app_wifi.h`:
```c
#ifndef APP_WIFI_H
#define APP_WIFI_H

#include "esp_err.h"

typedef void (*app_wifi_cb_t)(bool connected, const char *ip);

/** 启动 Wi-Fi STA 模式并连接 */
esp_err_t app_wifi_start(const char *ssid, const char *password);

/** 注册连接状态回调 */
void app_wifi_register_cb(app_wifi_cb_t cb);

/** 获取当前 IP 地址 */
const char *app_wifi_get_ip(void);

/** 检查 Wi-Fi 是否已连接 */
bool app_wifi_is_connected(void);

#endif
```

- [ ] **Step 2: 实现 app_wifi.c**

`qa_volc/main/app_wifi.c`:
```c
#include "app_wifi.h"
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "APP_WIFI";

static bool s_connected = false;
static char s_ip[16] = {0};
static app_wifi_cb_t s_cb = NULL;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting...");
        esp_wifi_connect();
        if (s_cb) s_cb(false, "");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_connected = true;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip);
        if (s_cb) s_cb(true, s_ip);
    }
}

esp_err_t app_wifi_start(const char *ssid, const char *password) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    return ESP_OK;
}

void app_wifi_register_cb(app_wifi_cb_t cb) { s_cb = cb; }

const char *app_wifi_get_ip(void) { return s_ip; }

bool app_wifi_is_connected(void) { return s_connected; }
```

- [ ] **Step 3: 提交**

---

### Task 4: main.c 框架 — 状态机 + 初始化序列

**Files:**
- Create: `qa_volc/main/include/qa_state_machine.h`
- Modify: `qa_volc/main/main.c`

- [ ] **Step 1: 创建状态机枚举**

`qa_volc/main/include/qa_state_machine.h`:
```c
#ifndef QA_STATE_MACHINE_H
#define QA_STATE_MACHINE_H

typedef enum {
    QA_STATE_BOOT = 0,
    QA_STATE_WIFI_CONNECT,
    QA_STATE_IDLE,
    QA_STATE_RECORDING,
    QA_STATE_ASR_WAIT,
    QA_STATE_LLM_WAIT,
    QA_STATE_ERROR,
} qa_state_t;

const char *qa_state_name(qa_state_t state);

#endif
```

- [ ] **Step 2: 实现 main.c 初始化序列**

`qa_volc/main/main.c` 包含以下初始化序列：
1. NVS flash init
2. BSP init (I2C, XL9555, SPI, LCD, audio, WS2812)
3. LVGL init + UI task create
4. TF card mount
5. config.ini parse
6. Wi-Fi connect
7. 进入 IDLE 状态

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "bsp_i2c.h"
#include "bsx_spi.h"
#include "xl9555.h"
#include "lcd_st7796.h"
#include "es8388.h"
#include "ws2812b.h"
#include "tf_sdcard.h"
#include "config_parser.h"
#include "app_wifi.h"
#include "qa_state_machine.h"
#include "task_qa_lvgl.h"
#include "task_audio_capture.h"
#include "task_ws2812.h"

static const char *TAG = "QA_MAIN";

void app_main(void) {
    ESP_LOGI(TAG, "===== QA Volc Boot =====");

    // 1. Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Init BSP peripherals
    bsp_i2c0_init();
    xl9555_init();
    board_keys_gpio_init();
    board_audio_gpio_init();
    board_lcd_gpio_init();
    board_ws2812_level_shifter_init();
    bsp_spi2_lcd_init();
    ESP_ERROR_CHECK(lcd_st7796_init());
    ESP_ERROR_CHECK(es8388_init(24, 60));  // gain=24, volume=60
    ws2812_init();

    // 3. Init LVGL UI
    lcd_lvgl_reserve_buffer();
    lcd_lvgl_task_create();

    // 4. Mount TF card
    esp_err_t mount_ret = tf_sdcard_mount();
    if (mount_ret != ESP_OK) {
        ESP_LOGE(TAG, "TF card mount failed");
        // UI shows "请插入TF卡" message
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 5. Parse config.ini
    config_t cfg;
    ret = config_parse("/sdcard/config.ini", &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "config.ini not found");
        // UI shows "缺少config.ini配置文件"
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 6. Start Wi-Fi
    const char *ssid = config_get_string(&cfg, "WIFI_SSID", NULL);
    const char *pass = config_get_string(&cfg, "WIFI_PASS", "");
    if (ssid == NULL) {
        ESP_LOGE(TAG, "WIFI_SSID not configured");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    app_wifi_start(ssid, pass);

    // 7. Start audio capture task
    audio_capture_task_create(&cfg);

    // 8. Start WS2812 task
    ws2812_task_create();

    ESP_LOGI(TAG, "Boot complete, entering IDLE");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

- [ ] **Step 3: 提交**

---

### Task 5: task_qa_lvgl — Q&A 对话界面

**Files:**
- Create: `qa_volc/main/task_qa_lvgl.h`
- Create: `qa_volc/main/task_qa_lvgl.c`

- [ ] **Step 1: 创建 task_qa_lvgl.h**

```c
#ifndef TASK_QA_LVGL_H
#define TASK_QA_LVGL_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t lcd_lvgl_reserve_buffer(void);
BaseType_t lcd_lvgl_task_create(void);

/** 在对话区添加用户消息 */
void qa_ui_add_user_msg(const char *text);

/** 在对话区添加助手消息 */
void qa_ui_add_assistant_msg(const char *text);

/** 在过程日志区添加一条日志 */
void qa_ui_add_log(const char *fmt, ...);

/** 更新底部状态栏文字 */
void qa_ui_set_status(const char *text);

/** 清除所有对话 */
void qa_ui_clear_all(void);

#endif
```

- [ ] **Step 2: 实现 task_qa_lvgl.c**

UI 布局（320×480）：
- 顶部状态栏: h=36, 显示"火山引擎智能问答" + Wi-Fi 状态
- 对话区: h=264, 可滚动，显示 [用户] 和 [助手] 消息
- 过程日志区: h=140, 可滚动，显示实时步骤
- 底部状态栏: h=40, 显示当前状态提示

关键 LVGL 组件：
- 对话区: `lv_textarea` 或 `lv_label` 在 `lv_page` 中
- 日志区: `lv_label` 在 `lv_page` 中，自动滚动到底部
- 状态栏: `lv_label` 固定底部

实现要点：
- 消息使用不同颜色前缀：`[用户]`=蓝色，`[助手]`=绿色
- 日志使用灰色文字
- 新消息追加后自动滚动到底部
- 使用 `lv_font_xiaozhi_cn_16.c` 作为中文字体

- [ ] **Step 3: 提交**

---

### Task 6: task_audio_capture — 按键录音 + WAV 存储

**Files:**
- Create: `qa_volc/main/task_audio_capture.h`
- Create: `qa_volc/main/task_audio_capture.c`

- [ ] **Step 1: 创建 task_audio_capture.h**

```c
#ifndef TASK_AUDIO_CAPTURE_H
#define TASK_AUDIO_CAPTURE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "config_parser.h"

/** 录音状态 */
typedef enum {
    AUDIO_IDLE,
    AUDIO_RECORDING,
} audio_capture_state_t;

/** 获取录音状态 */
audio_capture_state_t audio_capture_get_state(void);

/** 创建录音任务（含按键检测循环）*/
BaseType_t audio_capture_task_create(const config_t *cfg);

/** 获取最后一次录音的文件路径 */
const char *audio_capture_get_last_file(void);

#endif
```

- [ ] **Step 2: 实现核心逻辑**

核心循环：
```c
static void audio_capture_task(void *pv) {
    const config_t *cfg = (const config_t *)pv;
    int timeout_s = config_get_int(cfg, "AUDIO_TIMEOUT_S", 30);

    while (1) {
        bool key_level;
        xl9555_get_pin_level(KEY_PORT, KEY3_PIN, &key_level);

        if (key_level == 0) { // KEY3 pressed (low active)
            // Start recording
            s_state = AUDIO_RECORDING;
            qa_ui_set_status("录音中...");
            qa_ui_add_log("[MIC] 开始录音");

            // Create WAV file: /sdcard/AUDIO/YYYYMMDD_HHMMSS.wav
            char filepath[64];
            // ... (use time + RTC or esp_timer)

            // Open WAV, write header placeholder
            // I2S read loop:
            int elapsed = 0;
            while (elapsed < timeout_s) {
                xl9555_get_pin_level(KEY_PORT, KEY3_PIN, &key_level);
                if (key_level == 1) break; // KEY3 released

                // Read I2S data
                size_t bytes_read;
                i2s_channel_read(audio_rx_handle, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
                // Write to WAV file
                fwrite(buffer, 1, bytes_read, wav_file);

                vTaskDelay(pdMS_TO_TICKS(10));
                elapsed++;
            }

            // Finalize WAV (update header)
            // Close file
            s_state = AUDIO_IDLE;
            qa_ui_add_log("[MIC] 录音完成 (%ds)", elapsed);

            // Notify ASR task to start
            // xTaskNotifyGive(s_asr_task_handle);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

- [ ] **Step 3: 提交**

---

### Task 7: task_volc_asr — 火山引擎 ASR HTTP 客户端

**Files:**
- Create: `qa_volc/main/task_volc_asr.h`
- Create: `qa_volc/main/task_volc_asr.c`

- [ ] **Step 1: 创建 task_volc_asr.h**

```c
#ifndef TASK_VOLC_ASR_H
#define TASK_VOLC_ASR_H

#include "esp_err.h"
#include "config_parser.h"

/** 启动 ASR 任务 */
BaseType_t volc_asr_task_create(const config_t *cfg);

/** 提交 WAV 文件路径给 ASR 任务 */
esp_err_t volc_asr_submit(const char *wav_path);

#endif
```

- [ ] **Step 2: 实现 HTTP POST**

请求格式（已在 Python 验证）：
```c
// URL: https://openspeech.bytedance.com/api/v3/auc/bigmodel/recognize/flash
// Headers:
//   X-Api-Key: <api_key>
//   X-Api-Resource-Id: volc.seedasr.auc
//   X-Api-Request-Id: <uuid>
//   X-Api-Sequence: -1
// Body: JSON with audio.data (base64)
```

实现步骤：
1. 读取 WAV 文件 → base64 编码
2. 构建 JSON payload
3. `esp_http_client` POST 请求
4. 解析响应 JSON（递归查找 `text` 字段）
5. 识别结果通过队列发到 UI 任务

- [ ] **Step 3: 提交**

---

### Task 8: task_volc_llm — 火山引擎 LLM SSE 客户端

**Files:**
- Create: `qa_volc/main/task_volc_llm.h`
- Create: `qa_volc/main/task_volc_llm.c`

- [ ] **Step 1: 创建 task_volc_llm.h**

```c
#ifndef TASK_VOLC_LLM_H
#define TASK_VOLC_LLM_H

#include "esp_err.h"
#include "config_parser.h"

BaseType_t volc_llm_task_create(const config_t *cfg);

/** 提交问题文本给 LLM 任务 */
esp_err_t volc_llm_submit(const char *question);

#endif
```

- [ ] **Step 2: 实现 SSE 流式接收**

请求格式（已在 Python 验证）：
```c
// URL: <LLM_ENDPOINT> (from config.ini)
// Headers:
//   Authorization: Bearer <LLM_API_KEY>
//   Content-Type: application/json
// Body:
//   {
//     "model": "<LLM_MODEL>",
//     "stream": true,
//     "input": [{"role":"user","content":[{"type":"input_text","text":"..."}]}]
//   }
```

SSE 解析：
```c
// 逐行读取响应
// 查找 "data: " 前缀
// 解析 JSON event
// event.type == "response.output_text.delta" → 提取 delta 并累加
// event.type == "response.done" → 结束
// 每个 delta 通过队列发到 UI 任务逐字显示
```

使用 `esp_http_client` 的 `HTTP_EVENT_ON_DATA` 回调处理流式数据。

- [ ] **Step 3: 提交**

---

### Task 9: 状态机串联 — main.c 主循环

**Files:**
- Modify: `qa_volc/main/main.c`

- [ ] **Step 1: 实现状态机主循环**

```c
static qa_state_t s_state = QA_STATE_BOOT;

void app_main(void) {
    // ... (初始化代码从 Task 4) ...

    s_state = QA_STATE_IDLE;
    qa_ui_set_status("按住KEY3说话");
    qa_ui_add_log("[SYS] 系统就绪，等待输入");

    while (1) {
        // Check KEY4 → clear dialog
        bool key4_level;
        xl9555_get_pin_level(KEY_PORT, KEY4_PIN, &key4_level);
        if (key4_level == 0) {
            qa_ui_clear_all();
            qa_ui_add_log("[CLR] 对话已清除");
            vTaskDelay(pdMS_TO_TICKS(300)); // debounce
        }

        // Check KEY1 → show Wi-Fi info
        bool key1_level;
        xl9555_get_pin_level(KEY_PORT, KEY1_PIN, &key1_level);
        if (key1_level == 0) {
            qa_ui_add_log("[INFO] IP: %s", app_wifi_get_ip());
            vTaskDelay(pdMS_TO_TICKS(300));
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

- [ ] **Step 2: 提交**

---

### Task 10: 编译调试 + 烧录验证

- [ ] **Step 1: 首次编译修复**

```bash
cd qa_volc
idf.py build 2>&1
```
修复所有编译错误（拼写错误、缺少头文件等）。

- [ ] **Step 2: 烧录 + 串口监视**

```bash
idf.py flash monitor
```

观察启动日志：NVS → BSP init → TF 卡挂载 → config.ini 解析 → Wi-Fi 连接

- [ ] **Step 3: 功能测试**

1. 插入带 config.ini 的 TF 卡 → 上电 → 观察 Wi-Fi 是否连接成功
2. 按下 KEY3 → 说话 → 松开 → 观察日志区是否显示步骤
3. 等待 ASR 结果和 LLM 回答显示在对话区
4. 按下 KEY4 → 确认对话清除
5. 测试没有 TF 卡时的错误提示
