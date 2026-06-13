# 火山引擎智能问答系统 MVP 设计文档

## 概述

基于 LYIT_ESP32S3MB 开发板，利用现有 xiaozhi 项目的 BSP 驱动和外设驱动，
构建一个精简的 MVP 智能问答系统。用户按住 KEY3 录音提问，经火山引擎 ASR
转文字，发豆包大模型回答，结果在 TFT 屏幕以对话形式显示。

## 硬件资源分配

| 外设 | 用途 | 引脚/地址 |
|------|------|-----------|
| ES8388 音频编解码器 | 录音 (MIC) | I2C 0x10 + I2S_NUM_0 |
| ST7796 LCD (320×480) | Q&A 对话显示 | SPI2, CS=GPIO21, DC=GPIO1 |
| XPT2046 触摸 | 预留给交互扩展 | SPI2, CS=GPIO2, IRQ=GPIO8 |
| WS2812B × 4 | 状态指示 | GPIO0 (RMT) |
| TF 卡 | 对话日志 + 音频文件存储 | SPI2, CS=GPIO40 |
| KEY3 (XL9555 P0.6) | 按住录音 / 松开发送 | 低电平有效 |
| KEY4 (XL9555 P0.7) | 清除当前对话历史 | 低电平有效 |
| KEY1 (XL9555 P0.4) | Wi-Fi 状态查看/重连 | 低电平有效 |
| LED1 (XL9555 P1.4) | 系统运行指示 | 低电平亮 |
| LED2 (XL9555 P1.5) | 录音状态指示 | 低电平亮 |
| 扬声器 (SPK, XL9555 P0.0) | 预留给后续 TTS | 高电平使能 |

## 项目结构

```
qa_volc/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig
├── sdkconfig.defaults
├── components/
│   ├── bsp/                    # 从 xiaozhi 复制
│   │   ├── bsp_audio.c
│   │   ├── bsp_exti.c
│   │   ├── bsp_gpio.c
│   │   ├── bsp_gptimer.c
│   │   ├── bsp_i2c.c
│   │   ├── bsp_ledc.c
│   │   ├── bsp_spi.c
│   │   ├── bsp_uart.c
│   │   └── include/ (对应 .h)
│   └── device/                 # 从 xiaozhi 复制
│       ├── es8388.c
│       ├── lcd_st7796.c
│       ├── tf_sdcard.c
│       ├── ws2812b.c
│       ├── xl9555.c
│       └── include/ (对应 .h)
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── main.c                  # 入口 + 初始化序列
│   ├── config_parser.c / .h    # 新 - TF卡 config.ini 解析器
│   ├── app_wifi.c / .h         # 简化 Wi-Fi 连接 (STA 模式,凭据来自 config.ini)
│   ├── task_qa_lvgl.c / .h     # Q&A 滚动对话 UI (LVGL)
│   ├── task_audio_capture.c / .h # 按键录音 + WAV 文件管理
│   ├── task_volc_asr.c / .h    # 火山引擎 ASR HTTP 客户端
│   ├── task_volc_llm.c / .h    # 豆包大模型 HTTP/SSE 客户端
│   ├── task_ws2812.c / .h      # 从 xiaozhi 复用, WS2812 控制
│   ├── task_sdcard.c / .h      # 从 xiaozhi 复用, 对话日志
│   └── task_timer.c / .h       # 从 xiaozhi 复用, 周期定时器
└── managed_components/         # 从 xiaozhi 复制
tools/
    └── verify_api.py           # API 协议验证脚本（先跑通Python再移植）
    └── config.ini              # 配置文件模板（拷到 TF 卡根目录）
```

## 配置文件 (config.ini)

所有用户可配置参数存放在 TF 卡根目录 `/sdcard/config.ini` 中。
系统启动时由 `config_parser` 模块解析，无需重新编译即可修改。

### config.ini 格式

```ini
# ========================================
# 火山引擎智能问答 - 配置文件
# 存放路径：/sdcard/config.ini
# ========================================

# ----------------------------------------
# WiFi 配置（必填）
# ----------------------------------------
WIFI_SSID=your_wifi_ssid
WIFI_PASS=your_wifi_password

# ========================================
# 火山引擎 ASR 配置（语音识别）
# ========================================
# 新版控制台：控制台 → 语音识别 → API Key 管理
# UUID 格式，例如：423199ee-f156-411b-84d6-ff2469c54a34
ASR_API_KEY=your_asr_api_key
# 以下三项旧版鉴权方式（与新版二选一，留空表示不启用）
# ASR_APPID=
# ASR_ACCESS_KEY=
# ASR_SECRET_KEY=

# ========================================
# 火山引擎 豆包大模型 配置
# ========================================
# API Key 从火山引擎方舟平台获取
LLM_API_KEY=your_llm_api_key
# 【方式A】完整端点 URL（推荐）
LLM_ENDPOINT=https://ark.cn-beijing.volces.com/api/v3/chat/completions
# 【方式B】Base URL（与方式A二选一，代码自动拼接 /chat/completions）
# LLM_BASE_URL=https://ark.cn-beijing.volces.com/api/v3
# 模型名称（必填）
LLM_MODEL=doubao-pro-32k

# ========================================
# 系统配置（可选）
# ========================================
# 录音超时（秒，默认 30）
AUDIO_TIMEOUT_S=30
# 对话上下文保留轮数（默认 10）
MAX_DIALOG_ROUNDS=10
```

### 启动时读取流程

```
TF卡挂载
  ↓
解析 /sdcard/config.ini
  ↓
提取 WIFI_SSID / WIFI_PASS → 连 Wi-Fi
提取 ASR_API_KEY             → 初始化 ASR 客户端
提取 LLM_API_KEY             → 初始化 LLM 客户端
提取 AUDIO_TIMEOUT_S         → 录音超时设置
提取 MAX_DIALOG_ROUNDS       → 对话上下文轮数
  ↓
进入 IDLE 待机
```

Wi-Fi 凭据优先从 config.ini 读取；若 config.ini 不存在或解析失败，
则显示 "请将 config.ini 放入 TF 卡" 提示并停止。

## 核心状态机

```
BOOT
  ↓ (硬件初始化完成)
WIFI_CONNECT
  ↓ (Wi-Fi 连接成功)
IDLE ──────────────────────────────
  ↑    ↓ KEY3按下                  ↑ KEY4按下
  │  RECORDING                     │
  │    ↓ KEY3松开                  │
  │  ASR_WAIT                     │
  │    ↓ 收到 ASR 结果            │
  │  LLM_WAIT                     │
  │    ↓ 收到完整 LLM 回答        │
  └───────────────────────────────┘
```

各状态说明：
- **BOOT**: 初始化 BSP、外设、LVGL。挂载 TF 卡，解析 `/sdcard/config.ini`。
  TFT 显示"系统启动中..."。若 config.ini 不存在或解析失败，显示错误提示。
- **WIFI_CONNECT**: 从 config.ini 读取 Wi-Fi 凭据连接。成功后显示"Wi-Fi 已连接"。
- **IDLE**: 待机状态。TFT 底部状态栏显示"按住KEY3说话"。WS2812 蓝色呼吸。
- **RECORDING**: KEY3 按下时进入。LED2 亮起，LED1 呼吸。WS2812 红色常亮。
  I2S 采集音频写 WAV 文件。超时 30 秒自动结束。
- **ASR_WAIT**: 上传录音到火山引擎 ASR 接口。底部栏"识别中..."。
  WS2812 黄色闪烁。收到结果后屏幕显示用户文字。
- **LLM_WAIT**: 发送 ASR 文本到豆包大模型。底部栏"思考中..."。
  WS2812 黄色闪烁。流式接收 SSE 响应，逐字追加显示。
- **IDLE**: 回答完毕，回到待机。

**KEY4（清除）**：在任何状态下短按，清空屏幕对话区域，重置对话上下文，
回到干净的 IDLE 状态。

## 音频处理流程

### 录音参数
- 采样率：16000 Hz
- 位深：16-bit
- 声道：单声道 (Mono)
- 格式：PCM → WAV 封装

### 录音实现
1. KEY3 按下 → 打开新 WAV 文件 `/sdcard/AUDIO/YYYYMMDD_HHMMSS.wav`
2. I2S 从 ES8388 MIC 通道采集，写入 WAV 数据块
3. KEY3 松开 → 更新 WAV header（写入 data chunk 大小），关闭文件
4. 超时 30 秒自动结束（防止卡键）
5. 保留最近 50 个录音文件，超出删除最旧

## 火山引擎 API 对接

### ASR (语音识别)

**接口**: 火山引擎一句话识别 HTTP API
**端点**: `https://openspeech.bytedance.com/api/v1/asr`
**鉴权**: 新版控制台 API Key（UUID 格式），通过 `X-Api-Key` 请求头发送
**请求方式**: HTTP POST，`Content-Type: application/json`
**请求体**: JSON，音频数据经 base64 编码后放入 `audio.data` 字段
**返回**: JSON，`result[0].text` 为识别文本

## API 验证流程

> 在写任何 ESP32 代码之前，先用 Python 验证 API 全链路。

```bash
cd tools
pip install requests
python3 verify_api.py
```

验证流程：
1. **TTS** → 用火山引擎语音合成生成一段测试语音（"你好，请问今天的天气怎么样？"）
2. **ASR** → 将生成的音频送语音识别，得到文字
3. **LLM** → 将识别文字送豆包大模型，验证 SSE 流式输出

全程无需麦克风，跑通即代表所有 API 协议、鉴权、格式都正确，ESP32 侧只是做 C 语言移植。

### 豆包大模型 (LLM)

**接口**: 火山引擎豆包 Chat Completion API（兼容 OpenAI 格式）
**BASE URL**: `LLM_BASE_URL` 从 config.ini 读取，拼接 `/chat/completions` 得到完整端点
**鉴权**: `Authorization: Bearer <LLM_API_KEY>`
**请求方式**: HTTP POST，JSON body，`stream=true` 开启 SSE 流式输出
**参数**:
- `model`: 从 config.ini 中 `LLM_MODEL` 读取（如 `doubao-pro-32k`）
- `messages`: 系统提示 + 对话上下文
- `stream: true`（流式逐字输出）

**System Prompt**: "你是一个智能问答助手，请简洁准确地回答用户的问题。"

**上下文管理**: 保留最近 N 轮对话（N 可配置，默认 10 轮），支持连续对话。
KEY4 清除时重置上下文。

## 屏幕显示详细设计 (LVGL)

### 布局参数

```
┌────────────────────────────┐  y=0
│  ⚡ 火山引擎智能问答        │  状态栏 (h=36)
│  ● WiFi已连接 | 13:30:15   │  字体 16px
├────────────────────────────┤  y=36
│                            │
│  [用户] 今天天气怎样？     │
│  [助手] 今天天气晴朗...    │  对话区域 (h=264, 可滚动)
│  [用户] 帮我写一首诗      │  上方2/3
│  [助手] 好的，这是一首关于  │
│   春天的诗...              │
├────────────────────────────┤  y=300
│  [13:30:15] 🎤 录音3.2秒   │
│  [13:30:16] 📡 ASR请求中   │  过程日志 (h=140, 可滚动)
│  [13:30:18] ✅ 识别完成    │  下方1/3
│  [13:30:18] 🤖 LLM响应中   │  显示每一步的实时状态
│  [13:30:20] 💬 正在输出... │
├────────────────────────────┤  y=440
│  按住KEY3说话  KEY4清屏    │  底部状态栏 (h=40)
│  ● 待命中                  │  字体 14px
└────────────────────────────┘  y=480
```

### 过程日志定义

日志栏实时输出系统每一步操作，格式统一：

```
[时间戳] 图标 动作描述
```

**日志类型对照表**：

| 阶段 | 日志示例 | 说明 |
|------|---------|------|
| 录音开始 | `[13:30:10] 🎤 开始录音...` | KEY3按下时立即显示 |
| 录音结束 | `[13:30:13] 🎤 录音完成 (3.2s)` | 显示实际录音秒数 |
| ASR请求 | `[13:30:13] 📡 ASR请求发送...` | HTTP请求开始 |
| ASR成功 | `[13:30:15] ✅ 识别: "今天天气怎样？"` | 实时显示识别文字 |
| ASR失败 | `[13:30:15] ❌ ASR失败: 超时` | 错误信息直观 |
| LLM请求 | `[13:30:15] 🤖 LLM请求发送...` | HTTP请求开始 |
| LLM输出 | `[13:30:17] 💬 今天天气晴...` | 截取前N字 |
| LLM完成 | `[13:30:22] ✅ 回答完成 (7.2s)` | 总耗时 |
| 清除 | `[13:30:30] 🗑️ 对话已清除` | KEY4操作反馈 |
| 错误 | `[13:30:35] ❌ Wi-Fi已断开` | 异常提示 |

保留最近 20 条日志，超出自动滚动删除最旧。

### 对话显示规则
- 每条消息独立一行，前缀 `[用户]` 或 `[助手]` 使用不同颜色
- 消息自动换行，超过行高可垂直滚动
- 新消息追加后自动滚到底部
- 最多保留 100 条消息，超出删除最旧
- 字体使用现有 `lv_font_xiaozhi_cn_16.c`（中文 16px 字体）

## 编译与烧录

```bash
cd qa_volc
idf.py set-target esp32s3
idf.py build flash monitor
```

**首次使用配置步骤**：

1. 烧录固件到开发板
2. 将 TF 卡插入电脑，在根目录创建 `/sdcard/config.ini`
3. 填入 Wi-Fi 账号密码和火山引擎 API 密钥
4. 将 TF 卡插入开发板，重新上电
5. 系统会自动读取配置，连接 Wi-Fi 后进入问答模式

> 后续修改配置只需拔卡改 config.ini，无需重新编译烧录。

## 预留扩展点
1. **TTS 语音合成** — 已有 ES8388 播放通路，后续可加火山引擎 TTS API
2. **OTA 升级** — 分区表已有 `storage` 分区，可加 OTA 功能
3. **触摸交互** — XPT2046 已连接，后续可用触摸替代按键
4. **摄像头** — OV2640 已连接，后续可加图像问答功能

## 关键约束
- KEY0 与 TF 卡 CS 共用 GPIO40，使用 TF 卡时 KEY0 不可用
- GPIO1 复用于 LCD DC 和板载 LED，使用 LCD 时板载 LED 不可控
- 录音期间 Wi-Fi HTTP 请求同时工作，注意 PSRAM 充足（8MB Octal PSRAM）
