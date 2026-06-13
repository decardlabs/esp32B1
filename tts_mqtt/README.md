# test008 — ESP32-S3 TTS Player

将 kai-sound-tts 的 Web → PHP → Python → MQTT → ESP32 TTS 播报方案，适配到 **ESP32-S3 开发板（ES8388 音频编解码器）** 的完整工程。

## 系统架构

```
Web 控制台 → PHP API → Python Bridge → Volcengine TTS
                                ↓
                            MQTT Broker
                                ↓
                    ┌─── ESP32-S3 (test008) ───┐
                    │  WiFi → MQTT 订阅命令     │
                    │  → HTTP 拉取 WAV 音频流   │
                    │  → 32KB 环形缓冲          │
                    │  → I2S (Philips, 16bit)  │
                    │  → ES8388 编解码器 → 喇叭  │
                    └───────────────────────────┘
```

## 项目结构

```
test008/
├── CMakeLists.txt              # 顶层 CMake (ESP-IDF 固件)
├── partitions.csv              # 分区表
├── sdkconfig.defaults          # 默认配置 (ESP32-S3, 8MB PSRAM, 16MB Flash)
├── .gitignore
├── README.md
├── PLAN.md                     # 设计与实现文档
├── main/                       # ESP32 固件源码
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── board_pins.h            # 引脚定义 + WiFi/MQTT 配置
│   ├── i2c_bus.c/h             # I2C0 总线驱动
│   ├── xl9555.c/h              # GPIO 扩展芯片 (功放使能)
│   ├── es8388.c/h              # ES8388 音频编解码器
│   ├── audio_player.c/h        # I2S 播放引擎 + 32KB 环形缓冲
│   ├── http_stream.c/h         # HTTP WAV 流拉取 + 头解析
│   ├── wifi_mqtt.c/h           # WiFi + MQTT 客户端
│   ├── cmd_handler.c/h         # MQTT JSON 命令分发
│   └── main.c                  # 入口函数 (初始化编排)
└── server/                     # 服务端配套代码
    ├── php/                    # PHP Web + API
    │   ├── config.php          # MQTT + Python Bridge 配置
    │   ├── composer.json       # php-mqtt 依赖
    │   ├── tts_request_handler.php  # 断句、预生成、MQTT 下发
    │   └── public/
    │       ├── index.html      # Web 控制台 (含设备状态显示)
    │       └── api/
    │           ├── tts.php         # TTS 播报入口
    │           ├── command.php     # 设备命令入口
    │           ├── tts-status.php  # TTS 状态轮询
    │           └── device-status.php  # 设备在线状态 API
    ├── python/                 # Volcengine TTS Bridge
    │   ├── tts_bridge_config.py
    │   ├── tts_bridge_server.py
    │   ├── requirements.txt
    │   └── README.md
    └── image/                  # 硬件图片
```

## 硬件差异 (vs kai-sound-tts 原版)

| 项目 | 原版 (MAX98357A) | 本固件 (ES8388) |
|------|-----------------|-----------------|
| 芯片 | ESP32 | ESP32-S3 |
| 框架 | Arduino | ESP-IDF v5.5.1 |
| 音频芯片 | MAX98357A (I2S DAC+功放一体) | ES8388 (I2C 编解码器) |
| I2S DOUT | GPIO25 | GPIO10 |
| I2S BCLK | GPIO27 | GPIO46 |
| I2S LRC | GPIO26 | GPIO9 |
| I2S MCLK | 无 | GPIO3 (ES8388 需要) |
| 功放使能 | MAX98357A SD 接 GND | XL9555 P0.0 (低电平开启) |
| I2C | 无 | GPIO41(SDA) / GPIO42(SCL) |

## 固件：编译与烧录

### 环境要求

- ESP-IDF v5.5.1
- ESP32-S3 工具链

### 编译烧录

```bash
source ~/esp/esp-idf-v5.5.1-full/esp-idf-v5.5.1/export.sh
cd /Users/macairm5/Documents/esp32/test008

# 编译
idf.py build

# 烧录（按 BOOT + RESET 进入下载模式）
PORT=$(ls /dev/cu.* | rg 'usbmodem|wchusbserial' | head -n 1)
idf.py -p "$PORT" flash

# 查看日志 (Ctrl + ] 退出)
idf.py -p "$PORT" monitor
```

### 固件配置

烧录前修改 `main/board_pins.h`：

```c
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASSWORD   "你的WiFi密码"
#define MQTT_BROKER     "你的服务器IP"
#define MQTT_PORT       1883
#define MQTT_USERNAME   "MQTT用户名"
#define MQTT_PASSWORD   "MQTT密码"
#define DEVICE_ID       "esp32_01"
```

## 服务端：部署指南

服务端需要三部分：Mosquitto、Python Bridge、PHP Web。

### Mosquitto (MQTT Broker)

```bash
# 安装
apt install mosquitto mosquitto-clients

# 创建密码
mosquitto_passwd -c /etc/mosquitto/passwd esp32_user

# 配置 (conf.d/esp32-tts.conf)
listener 1883 0.0.0.0
allow_anonymous false
password_file /etc/mosquitto/passwd

# 开放防火墙
ufw allow 1883/tcp
```

### Python TTS Bridge

```bash
cd server/python
pip install -r requirements.txt

# 编辑配置
# 修改 tts_bridge_config.py 中的 VOLCENGINE 密钥和 TTS_PUBLIC_BASE_URL

# 启动
python3 tts_bridge_server.py --host 0.0.0.0 --port 9100
```

### PHP Web

```bash
cd server/php
composer install

# 将 public/ 目录设为 Web 服务器根目录
# 配置 Nginx 指向 server/php/public/
# 修改 config.php 中的 MQTT 用户名密码
```

### 启动顺序

1. Mosquitto
2. Python TTS Bridge
3. PHP + Web 服务
4. ESP32-S3 上电

## 设备在线状态

网页控制台每 5 秒自动轮询 `api/device-status.php`，获取 ESP32 的 MQTT 保留状态消息，以绿色/红色圆点显示设备在线/离线。

## MQTT 协议

### 订阅
```
device/{DEVICE_ID}/command
```

### 命令

| 命令 | 参数 | 说明 |
|------|------|------|
| `tts` | `url`, `text_preview` | 播报 TTS 音频流 |
| `play` | `url` | 播放任意音频 URL |
| `pause` | — | 暂停 |
| `resume` | — | 继续 |
| `stop` | — | 停止 |
| `volume` | `value` (0-100) | 音量 |
| `tone` | `freq`, `duration` | 测试音 |
| `status` | — | 刷新状态 |

### 发布
- `device/{DEVICE_ID}/status` — 完整状态 (retained, 含遗嘱消息)
- `device/{DEVICE_ID}/heartbeat` — 心跳 (10 秒间隔)

## 限制

- **仅支持 WAV 格式**（未集成 MP3 解码器，播放前需在网页将格式切为 wav）
- GPIO46 (I2S BCLK) 为 ESP32-S3 RTC 域引脚，驱动能力偏弱，可能导致轻微底噪（硬件设计限制）
- 公网模式下音频流延迟受网络质量影响

## 版本

当前版本：v1.0 — 主链路跑通，设备在线状态可监测。
