# test004 — ESP32-S3 USB Mass Storage + ST7796 LCD 演示

基于 ESP-IDF 的 ESP32-S3 演示项目，展示 **ST7796 TFT 静态文字显示** + **TinyUSB MSC（U 盘模式）** 的组合使用。连接电脑后，TF 卡以 U 盘形式出现；同时 LCD 静态显示调试文字。

## 功能

- **ST7796 SPI TFT 显示**：上电后在 320×480 屏幕上显示静态文字
- **TF 卡初始化**：SPI2 总线挂载 MicroSD 卡（FATFS）
- **TinyUSB MSC 设备**：将 TF 卡作为 USB U 盘暴露给电脑主机
- LCD 与 TF 卡共享 SPI2 总线，SD 卡初始化失败时系统仍可正常运行（非致命错误）

## 硬件引脚

| 功能 | GPIO | 说明 |
|------|------|------|
| SPI2 MOSI | GPIO11 | LCD 和 TF 卡共用 |
| SPI2 CLK | GPIO12 | |
| SPI2 MISO | GPIO13 | |
| LCD CS | GPIO21 | |
| LCD DC | GPIO1 | |
| TF 卡 CS | GPIO40 | |
| I2C0 SDA | GPIO41 | XL9555 控制 LCD 背光/复位 |
| I2C0 SCL | GPIO42 | |
| XL9555 地址 | 0x20 | |
| LCD RST | XL9555 P1.3 | 通过 GPIO 扩展芯片控制 |
| LCD 背光 | XL9555 P1.1 | 通过 GPIO 扩展芯片控制 |

LCD 分辨率：**320 × 480**

## 项目结构

```
test004/
├── CMakeLists.txt
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── board_pins.h        # GPIO 引脚定义
    ├── main.c              # 入口：LCD → SD → USB MSC 初始化
    ├── lcd_st7796.c/h      # ST7796 SPI LCD 驱动（含 XL9555 I2C 初始化）
    ├── font_8x16.h         # 8×16 点阵字体
    ├── sd_card.c/h         # TF 卡 SPI 挂载（FATFS）
    └── tusb_msc.c/h        # TinyUSB Mass Storage Class 设备
```

## 启动流程

```
上电
 │
 ├─ Step 1: 初始化 I2C + XL9555 + SPI2 → LCD 显示静态文字
 │
 ├─ Step 2: 在相同 SPI2 总线上初始化 TF 卡（失败则跳过）
 │
 └─ Step 3: 启动 TinyUSB MSC，将 TF 卡暴露为 U 盘
           TinyUSB 在后台运行，主循环空转
```

## 编译与烧录

```bash
# 设置 ESP-IDF 环境（v5.5.1）
source ~/esp/esp-idf-v5.5.1/export.sh

# 进入项目目录
cd /Users/macairm5/Documents/esp32/test004

# 编译
idf.py build

# 烧录
PORT=$(ls /dev/cu.* | rg 'usbmodem|wchusbserial|usbserial' | head -n 1)
idf.py -p "$PORT" flash

# 监控日志
idf.py -p "$PORT" monitor
```

## 软件环境

- **ESP-IDF**: v5.5.1
- **芯片**: ESP32-S3
- **依赖**: TinyUSB（ESP-IDF 内置），FATFS（ESP-IDF 内置）

## 注意事项

- TF 卡为可选外设，不插卡时 LCD 仍可正常工作，USB MSC 功能不可用
- SPI2 总线需先由 LCD 初始化（设置较大的 `max_transfer_sz`），之后 TF 卡设备再挂载到同一总线
- 本项目是 test005（语音朗读器）的前置基础演示，test005 在此基础上增加了 LVGL、TTS、按键等功能
