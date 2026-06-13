# Changelog

## v0.2.0 (2026-06-13)

### 🧹 仓库整理

- 扁平化仓库结构：移除嵌入的子模块 .git，所有代码直接纳入版本管理
- 清理 IDE/AI 工具目录（.codegraph、.workbuddy）不再追踪
- 清理构建产物（build 目录、build_stale、managed_components）
- 清理二进制工具文件（*.exe）
- 添加 .gitattributes：统一文本文件的 LF 换行符
- 完善 .gitignore：覆盖 IDE、AI 工具、构建产物、二进制文件
- 移除 pico-fido 遗留的 .gitmodules 配置

## v0.1.0 (2026-06-13)

### ✨ 初始发布

收录基于 LYIT_ESP32S3MB 开发板的 ESP32-S3 示例合集：

**基础外设**
- xl9555_demo — XL9555 GPIO 扩展驱动
- multi_periph — 多外设综合演示（LCD / OLED / WS2812）
- usb_msc_lcd — USB Mass Storage + LCD 显示
- ble_kbd — BLE HID 蓝牙键盘

**语音应用**
- reader_v1 — 语音朗读器（本地 TTS）
- reader_v2 — 语音朗读器（本地 + 云端 TTS，自动降级）
- tts_reader — TTS 朗读基础版
- tts_mqtt — MQTT 远程 TTS 播报

**智能应用**
- matter_light — ESP-Matter 智能灯
- xiaozhi — 小智 AI 综合终端

**文档**
- board_docs — 开发板硬件设计参考

> 同步收录配套工具：kai-sound-tts（云端 TTS 方案）、voicetest（Web TTS 工具）、esp32code（入门例程）
