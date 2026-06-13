# test006 - ESP32-S3 BLE HID Keyboard + TFT Status

本项目是基于 ESP-IDF 的 BLE HID 键盘示例，支持：
- iPhone/电脑蓝牙连接后，按键交替上传字符 0 / 1
- 蜂鸣器按键反馈（含消抖）
- TFT 状态显示（蓝底白字、左对齐、上下居中）

## 1. 主要功能

- BLE HID 键盘
  - 设备名：ESP32S3-HID-KBD
  - 连接后按键发送 HID 键值：0 / 1 交替
- 本地按键
  - KEY0: GPIO40（低电平按下）
  - BOOT: GPIO0（低电平按下）
- 蜂鸣器
  - 通过 XL9555 控制，按键时短鸣
- TFT 状态页
  - 显示蓝牙连接状态、安全状态、最后按键、按键计数、蜂鸣可用状态

## 2. 硬件与引脚

请先阅读完整引脚说明：PIN_MAP.md。

本项目关键连接：
- I2C0: SDA GPIO41, SCL GPIO42
- XL9555: I2C 地址 0x20
- 蜂鸣相关:
  - P0.0: SPK_SD
  - P0.1: BEEP
- LCD ST7796:
  - SPI2, CS GPIO21, DC GPIO1
  - RST 由 XL9555 P1.3 控制
  - 背光由 XL9555 P1.1 控制

## 3. 软件环境

- ESP-IDF: v5.5.1
- 芯片: ESP32-S3
- 推荐串口: 使用当前系统识别到的 /dev/cu.usbmodemxxx

## 4. 编译与烧录

在项目根目录执行：

```bash
source ~/esp/esp-idf-v5.5.1-full/esp-idf-v5.5.1/export.sh
idf.py build
```

自动选择串口并烧录：

```bash
PORT=$(ls /dev/cu.* | rg 'usbmodem|wchusbserial|usbserial' | head -n 1)
idf.py -p "$PORT" flash
```

查看日志：

```bash
idf.py -p "$PORT" monitor
```

## 5. 使用方法

1. 上电后，手机或电脑搜索并连接蓝牙设备 ESP32S3-HID-KBD。
2. 连接成功后，按 KEY0 或 BOOT。
3. 每次按下会交替发送字符 0 / 1。
4. TFT 会实时更新：
   - BT 连接状态
   - SECURITY 状态
   - LAST KEY
   - KEY COUNT
   - BEEP 状态

## 6. 代码结构

- main/main.c
  - BLE HID 初始化与事件回调
  - 按键消抖与发送逻辑
  - 蜂鸣控制
  - TFT 状态渲染
- main/esp_hid_gap.c
  - NimBLE GAP 与安全相关流程
- components/bsp
  - I2C/SPI 基础驱动
- components/device
  - XL9555 与 ST7796 驱动

## 7. 当前稳定行为（已验证）

- 蓝牙连接与按键上传正常
- 0/1 交替发送正常
- 蜂鸣反馈正常
- TFT 蓝底白字显示正常
- 按键计数与最后按键实时更新正常

## 8. 常见问题

### Q1: 蓝牙能连上，但按键没有输入

建议检查：
- 是否真的连接到了 ESP32S3-HID-KBD（不是历史缓存设备）
- 串口日志是否出现按键日志与 HID 发送日志
- 手机输入焦点是否在可输入文本框

### Q2: TFT 显示异常或花屏

建议检查：
- LCD 供电与排线
- XL9555 初始化是否成功
- SPI2 初始化是否成功
- 是否误改了高频全屏刷新逻辑

### Q3: 烧录口找不到

建议：
- 重新插拔 USB
- 重新执行串口探测命令
- 关闭占用串口的监视器后再 flash

## 9. 维护建议

- 功能稳定后，优先小步修改并单项验证（BLE/按键/蜂鸣/TFT）。
- 先验证编译，再烧录，再做连接与按键回归测试。
- 每次改动建议保留一份可回退的稳定版本记录。
