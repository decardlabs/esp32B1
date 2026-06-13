# test008 — ESP32-S3 TTS Player (kai-sound-tts 硬件适配)

## 目标

将 [kai-sound-tts](../../kai-sound-tts) 的 Web → PHP → Python → MQTT → ESP32 TTS 播报方案，
适配到 ESP32-S3 开发板（ES8388 音频编解码器，XL9555 GPIO 扩展）。

## 与 kai-sound-tts 的硬件差异

| 项目 | 原版 (MAX98357A) | 开发板 (ES8388) |
|------|-----------------|-----------------|
| 芯片 | ESP32 | ESP32-S3 |
| 框架 | Arduino | ESP-IDF v5.5.1 |
| 音频芯片 | MAX98357A (I2S DAC+功放) | ES8388 (I2C 编解码器) |
| I2S DOUT | GPIO25 | GPIO10 |
| I2S BCLK | GPIO27 | GPIO46 |
| I2S LRC | GPIO26 | GPIO9 |
| I2S MCLK | 无 | GPIO3 (ES8388 需要) |
| 功放使能 | MAX98357A SD 接 GND | XL9555 P0.0 (低电平开启) |
| I2C | 无 | GPIO41(SDA) / GPIO42(SCL) |

与 [test005](../test005) 的硬件差异：test008 专注于 MQTT 远程播报，不需要 LCD/SD卡/USB MSC/按键。

## 架构

```
MQTT 命令 → ESP32-S3 → 解析命令
                         → HTTP 拉取 WAV 音频流
                         → Ring Buffer (32KB)
                         → I2S (Philips, 16bit, mono)
                         → ES8388 → 喇叭
```

## 项目文件结构

```
test008/
├── CMakeLists.txt              # 顶层 CMake
├── partitions.csv              # 分区表 (14MB app)
├── sdkconfig.defaults          # 默认配置 (ESP32-S3, 8MB PSRAM, 16MB Flash)
├── .gitignore
├── PLAN.md                     # 本文档
└── main/
    ├── CMakeLists.txt           # 组件注册
    ├── idf_component.yml        # 依赖管理
    ├── board_pins.h             # 引脚定义 + WiFi/MQTT 配置
    ├── i2c_bus.c/h              # I2C0 总线 (SDA=41, SCL=42)
    ├── xl9555.c/h               # GPIO 扩展芯片 (功放使能)
    ├── es8388.c/h               # ES8388 音频编解码器驱动
    ├── audio_player.c/h         # I2S 播放引擎 + 环形缓冲区 + WAV 解析
    ├── http_stream.c/h          # HTTP 流拉取
    ├── wifi_mqtt.c/h            # WiFi 连接 + MQTT 客户端
    ├── cmd_handler.c/h          # MQTT 命令解析与分发
    └── main.c                   # 入口 + 初始化编排
```

## 状态

| 模块 | 状态 | 说明 |
|------|------|------|
| 硬件驱动层 (I2C/XL9555/ES8388) | ✅ 已完成 | 从 test005 移植，已验证 |
| I2S 音频播放 | ✅ 已完成 | Philips 格式, 16bit, mono, 32KB ring buffer |
| WAV 流播放 | ✅ 已完成 | HTTP 拉流 → 解析 WAV 头 → 环形缓冲区 → I2S |
| WiFi + MQTT | ✅ 已完成 | 自动重连, 订阅命令主题, 发布状态/心跳 |
| 命令处理 | ✅ 已完成 | play/pause/resume/stop/volume/tone/status/tts |
| 测试音 | ✅ 已完成 | I2S 直接生成正弦波 |
| MP3 解码 | ⏳ 待实现 | 需要集成 minimp3 解码器(当前 WAV 格式可用) |
| LCD 状态显示 | 📝 未来增强 | 可复用 test005 的 LVGL 驱动 |
| 按键控制 | 📝 未来增强 | 可复用 test005 的 XL9555 按键驱动 |

## MQTT 协议 (兼容 kai-sound-tts)

### 订阅主题
```
device/{DEVICE_ID}/command
```

### 命令格式 (JSON)
```json
{"cmd": "tts", "url": "http://...", "text_preview": "你好"}
{"cmd": "play", "params": {"url": "http://..."}}
{"cmd": "pause"}
{"cmd": "resume"}
{"cmd": "stop"}
{"cmd": "volume", "params": {"value": 80}}
{"cmd": "tone", "params": {"freq": 1000, "duration": 1200}}
{"cmd": "status"}
```

### 发布主题
- `device/{DEVICE_ID}/status` — 完整状态 (retained, 含 ip/rssi/volume/play_state)
- `device/{DEVICE_ID}/heartbeat` — 心跳 (10 秒间隔, wifi_rssi/volume/play_state/free_heap)

## 编译与烧录

```bash
# 设置 ESP-IDF 环境 (v5.5.1)
source ~/esp/esp-idf-v5.5.1-full/esp-idf-v5.5.1/export.sh
cd /Users/macairm5/Documents/esp32/test008

# 编译
idf.py build

# 烧录 + 串口监视器
PORT=$(ls /dev/cu.* | rg 'usbmodem|wchusbserial' | head -n 1)
idf.py -p "$PORT" flash monitor

# 仅 monitor
idf.py -p "$PORT" monitor
```

## 配置

烧录前必须修改 `main/board_pins.h` 中的配置常量：

| 常量 | 说明 | 默认值 |
|------|------|--------|
| `WIFI_SSID` | WiFi 名称 | `"YOUR_WIFI_SSID"` |
| `WIFI_PASSWORD` | WiFi 密码 | `"YOUR_WIFI_PASSWORD"` |
| `MQTT_BROKER` | MQTT 地址 | `"YOUR_MQTT_HOST"` |
| `MQTT_PORT` | MQTT 端口 | `1883` |
| `MQTT_USERNAME` | MQTT 用户名 | `""` (留空则不填) |
| `MQTT_PASSWORD` | MQTT 密码 | `""` (留空则不填) |
| `DEVICE_ID` | 设备 ID | `"esp32_01"` |

## 启动顺序

1. Mosquitto MQTT broker
2. Python TTS bridge (端口 9100, 需 `TTS_PUBLIC_BASE_URL` 指向 ESP32 可访问的地址)
3. PHP + Web 服务
4. ESP32-S3 上电
5. 打开 Web 控制台 → 输入文本 → "发送播报"

## 关键改动说明

### 初始化流程 (main.c)
```
I2C (GPIO41/42)
  → XL9555 (功放使能 P0.0=低电平)
  → ES8388 (I2C 写入 DAC 寄存器配置)
  → I2S0 (MCLK=3, BCLK=46, LRCK=9, DOUT=10)
  → WiFi + MQTT (订阅命令主题, 发布状态)
```

### 音频流处理 (http_stream.c + audio_player.c)
```
HTTP GET → 读取 256 bytes → 解析 WAV 头 (RIFF/WAVE/data 块)
  → 获取 sample_rate + data_offset
  → audio_player_start_stream(rate)
  → 循环读取 HTTP 剩余数据 → audio_player_feed_pcm()
  → I2S DMA (Philips, 16bit, mono) → ES8388 → 喇叭
```

### 音量映射
MQTT 命令的 `volume` 为 0-100 百分比，内部转换为 ES8388 DAC 寄存器值：
- 0% → -96 dB (静音)
- 50% → -24 dB
- 80% → -12 dB (推荐默认)
- 100% → 0 dB (最大)

## 已知问题

- GPIO46 (I2S BCLK) 为 ESP32-S3 RTC 域引脚，驱动能力弱，可能导致音频底噪（同 test005 的硬件限制）
- 当前仅支持 WAV 格式播放。如需 MP3，需配置 PHP/Python 侧使用 WAV 输出（`format: "wav"`）
