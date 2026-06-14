# ESP32-S3 Demo Collection

基于 **LYIT_ESP32S3MB 开发板** 的 ESP-IDF 综合示例合集，覆盖从基础外设驱动到语音、蓝牙、Matter 等完整应用方案。

> 开发板主控：ESP32-S3（16MB Flash + 8MB Octal PSRAM）
> 关键外设：ST7796 LCD、XL9555 GPIO 扩展、ES8388 音频编解码、WS2812B 灯带

## 项目清单

| 目录 | 说明 |
|------|------|
| [xl9555_demo](xl9555_demo) | XL9555 GPIO 扩展基础驱动：按键/ LED / 蜂鸣器 |
| [board_docs](board_docs) | 开发板硬件设计资料（原理图 / BOM / 引脚映射） |
| [multi_periph](multi_periph) | 多外设综合演示（LCD / OLED / WS2812，menuconfig 切换） |
| [usb_msc_lcd](usb_msc_lcd) | USB Mass Storage（U 盘模式）+ ST7796 LCD 显示 |
| [reader_v1](reader_v1) | 语音朗读器 v1：USB 拷贝 + 本地 TTS 朗读 |
| [ble_kbd](ble_kbd) | BLE HID 蓝牙键盘 + TFT 状态显示 |
| [matter_light](matter_light) | ESP-Matter 智能灯：触屏 + LVGL + WS2812 |
| [tts_mqtt](tts_mqtt) | MQTT 远程 TTS 播报（Web → MQTT → ESP32 播报） |
| [reader_v2](reader_v2) | 语音朗读器 v2：本地 + 云端 TTS，自动降级 |
| [xiaozhi](xiaozhi) | 小智 AI 综合终端：人脸识别 / 摄像头 / 云端 TTS |
| [qa_volc](qa_volc) | 火山引擎智能问答：语音识别 + LLM 大模型 + TTS 语音合成 |
| [tts_reader](tts_reader) | TTS 朗读基础版：TF 卡 TXT 朗读 + 音量 / 语速调节 |
| [esp32code](esp32code) | ESP-IDF 入门学习例程（GPIO / EXTI / 任务等） |
| [kai-sound-tts](kai-sound-tts) | Web → PHP → Python → MQTT → ESP32 云端 TTS 方案 |
| [voicetest](voicetest) | 多引擎 Web TTS 工具（讯飞 / 火山引擎） |
| [pico-fido](pico-fido) | RP2040 FIDO2/U2F 安全密钥 |
| [docs](docs) | 开发文档和资料 |

## 快速开始

```bash
# 以 reader_v2 为例
cd reader_v2
idf.py build flash monitor
```

各项目详细用法见对应目录下的 README.md。

## 版本

当前版本：**v0.2.0** — 新增 qa_volc 智能问答项目。[更新日志](docs/CHANGELOG.md)
