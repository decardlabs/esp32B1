# Cloud TTS: ESP32-S3 豆包語音合成

## Overview

在 ESP32-S3 Voice Reader 專案（test005）基礎上建立新專案 test009，
新增雲端 TTS 功能：通過 WiFi 連接火山引擎 TTS API，
將 SD 卡上的文字合成語音並在本地播放。

保留現有本地 esp_tts 引擎作為離線降級方案。

## 配置文件 (`/sdcard/config.ini`)

INI 格式，key=value，`#` 開頭為註釋：

```ini
# WiFi
WIFI_SSID=MyNetwork
WIFI_PASS=password123

# Doubao TTS
TTS_APPID=12345678
TTS_TOKEN=your_access_token
TTS_VOICE=zh_female_zhimi
```

## 新增模組

### app_config

- 功能：讀取 `/sdcard/config.ini`，解析 key=value 對
- 依賴：SD 卡 FATFS（已由 sd_card.c 掛載）
- 介面：

```c
typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char tts_appid[32];
    char tts_token[256];
    char tts_voice[32];
} app_config_t;

esp_err_t app_config_load(app_config_t *cfg);
```

### app_wifi

- 功能：WiFi 站模式連接，事件驅動，自動重連
- 依賴：esp_wifi, esp_netif, esp_event
- 介面：

```c
typedef void (*wifi_cb_t)(bool connected);

esp_err_t app_wifi_init(void);
esp_err_t app_wifi_connect(const char *ssid, const char *pass);
void app_wifi_disconnect(void);
bool app_wifi_is_connected(void);
void app_wifi_set_callback(wifi_cb_t cb);
```

### app_cloud_tts

- 功能：HTTPS POST 到火山引擎 TTS API，流式接收 PCM，經環形緩衝區餵給 I2S
- 依賴：esp_http_client, 環形緩衝區, I2S（共享 app_tts 的 tx_handle）
- API 請求：

```
POST https://openspeech.bytedance.com/api/v1/tts
Authorization: Bearer <token>
Content-Type: application/json

{
  "app": {
    "appid": "<appid>",
    "token": "<token>",
    "cluster": "volcano_tts"
  },
  "user": { "uid": "esp32s3_001" },
  "audio": {
    "voice_type": "<voice>",
    "encoding": "pcm",
    "speed_ratio": 1.0,
    "volume_ratio": 1.0,
    "pitch_ratio": 1.0
  },
  "request": {
    "reqid": "<uuid>",
    "text": "<sentence_text>",
    "text_type": "plain",
    "operation": "query"
  }
}
```

- 響應：HTTP 200，body 為原始 PCM 二進制（16kHz, 16-bit, mono, little-endian）
- 流式處理：esp_http_client `on_data` 回調 → 寫入環形緩衝區
- 環形緩衝區：32KB，雙指針（寫入/讀出）
- I2S 播放任務：從環形緩衝區讀取 → i2s_channel_write → ES8388
- 啟動條件：環形緩衝區 > 50% 時開始播放（防 buffer underrun）
- 停止條件：HTTP 完成 + 緩衝區耗盡

介面：

```c
typedef void (*cloud_tts_done_cb_t)(void);

esp_err_t app_cloud_tts_init(void);
esp_err_t app_cloud_tts_speak(const char *text, cloud_tts_done_cb_t cb);
void app_cloud_tts_stop(void);
bool app_cloud_tts_is_busy(void);
```

## 修改模組

### app_tts (app_tts.c/h)

- 保留所有本地 esp_tts 代碼不變
- 新增 `app_tts_set_channel(tts_channel_t ch)` 選擇通道：
  - `TTS_CHANNEL_LOCAL` — 本地 esp_tts（現有行為）
  - `TTS_CHANNEL_CLOUD` — 雲端豆包 TTS
  - `TTS_CHANNEL_AUTO` — WiFi 已連用雲端，否則降級本地
- `app_tts_speak()` / `app_tts_speak_cb()` 根據當前通道路由到對應引擎
- `app_tts_is_busy()` 報告任一通道繁忙

### main.c

- 初始化順序增加：SD card mount → config load → WiFi init → connect
- 閱讀模式分為「本地閱讀」和「雲端閱讀」
- 主菜單按 KEY2 進入本地閱讀，長按進入雲端閱讀（或按鍵切換）
- 顯示 WiFi 狀態圖標

### app_display (app_display.c/h)

- 新增雲端閱讀模式狀態顯示
- 顯示 WiFi 連接狀態
- 顯示雲端 TTS 播放進度

## 數據流

```
SD 卡 .txt
    → main.c 分句
    → app_tts_speak(sentence)
        → 判斷通道
            → LOCAL: 本地 esp_tts（現有邏輯）
            → CLOUD:
                → app_cloud_tts_speak(text, cb)
                    → HTTP POST (HTTPS)
                    → on_data 回調: 寫入 ring buffer
                    → playback task: ring buffer → I2S → ES8388
                    → cb() when done
```

## 關鍵技術點

1. **TLS 內存**：HTTPS 需要 ~30-40KB heap，ESP32-S3 PSRAM 充足
2. **環形緩衝區大小**：32KB ≈ 1 秒 PCM @ 16kHz 16-bit，足夠吸收網絡抖動
3. **I2S 共享**：雲端和本地 TTS 共用同一 I2S 通道，播放前切換
4. **錯誤處理**：WiFi 斷開/API 超時 → 自動降級到本地 TTS
5. **reqid 生成**：簡單遞增計數器或毫秒時間戳

## 專案結構

```
test009/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    ├── board_pins.h
    ├── main.c              # [修改] 增加 WiFi/config init + 雲端模式
    ├── app_config.c/h      # [新增]
    ├── app_wifi.c/h        # [新增]
    ├── app_cloud_tts.c/h   # [新增]
    ├── app_display.c/h     # [修改] 增加 WiFi/雲端狀態顯示
    ├── app_buttons.c/h     # 不變
    ├── app_tts.c/h         # [修改] 通道選擇 + 降級邏輯
    ├── lcd_st7796.c/h      # 不變
    ├── bsp_spi.c/h         # 不變
    ├── sd_card.c/h         # 不變
    ├── tusb_msc.c/h        # 不變
    ├── i2c_bus.c/h         # 不變
    ├── xl9555.c/h          # 不變
    ├── es8388.c/h          # 不變
    └── lv_conf.h           # 不變
```
