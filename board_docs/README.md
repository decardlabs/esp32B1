# test002 — ESP32-S3 硬件设计参考文档

本目录不包含可编译的固件代码，而是 **LYIT_ESP32S3MB 开发板的硬件设计参考资料**，包括原理图、BOM、引脚映射以及固件 GPIO 初始化参考。

## 目录内容

| 文件 | 说明 |
|------|------|
| `ESP32S3_BOM.csv` | 物料清单（BOM）— 元器件型号、数量、位号 |
| `ESP32S3_netlist.net` | 网表文件（原理图导出） |
| `ESP32S3电路图.pdf` | 完整原理图 PDF |
| `ESP32S3电路图.pdf.png` | 原理图封面预览图 |
| `XL9555 Datasheet Rev2.3.PDF` | XL9555 I2C GPIO 扩展芯片数据手册 |
| `pin_mapping.json` | 原始引脚映射（所有外设 GPIO 分配） |
| `pin_mapping_firmware.json` | 固件实际使用的引脚映射（经过验证） |
| `firmware_gpio_init_reference.md` | 固件 GPIO 初始化参考（各外设初始化方式） |
| `pin_mapping_verification_checklist.md` | 引脚映射验证清单 |
| `page1.txt` / `page2.txt` / `page3.txt` | 其他参考笔记 |

## 主要外设引脚汇总

### I2C0 总线（共用）

| 信号 | GPIO |
|------|------|
| SDA | GPIO41 |
| SCL | GPIO42 |
| XL9555 地址 | 0x20 |
| ES8388 地址 | 0x10 |

### SPI2 总线（LCD + TF 卡共用）

| 信号 | GPIO |
|------|------|
| MOSI | GPIO11 |
| MISO | GPIO13 |
| CLK | GPIO12 |
| LCD CS | GPIO21 |
| LCD DC | GPIO1 |
| TF CS | GPIO40 |

### I2S0（音频 ES8388）

| 信号 | GPIO |
|------|------|
| MCLK | GPIO3 |
| BCLK | GPIO46 |
| LRCK | (见 firmware_gpio_init_reference.md) |
| SDIN / SDOUT | (见 firmware_gpio_init_reference.md) |

### 摄像头（OV2640，SPI2 / I2C 共用）

| 信号 | GPIO |
|------|------|
| D0–D5 | GPIO4–GPIO8, GPIO16 |
| D6, D7 | GPIO17, GPIO18 |
| PCLK | GPIO45 |
| VSYNC | GPIO47 |
| HREF | GPIO48 |
| OV_SCL | GPIO38 |
| OV_SDA | GPIO39 |
| OV_RESET | XL9555 P0_3 |
| OV_PWDN | XL9555 P0_2 |

### XL9555 引脚分配

| XL9555 引脚 | 功能 |
|------------|------|
| P0.0 | SPK_EN（功放使能） |
| P0.1 | BEEP（蜂鸣器） |
| P0.2 | OV_PWDN（摄像头断电） |
| P0.3 | OV_RESET（摄像头复位） |
| P0.4 | KEY1 |
| P0.5 | KEY2 |
| P0.6 | KEY3 |
| P0.7 | KEY4 |
| P1.0 | WS2812 电平转换器使能 |
| P1.1 | LCD 背光 |
| P1.3 | LCD RST |
| P1.4 | LED1 |
| P1.5 | LED2 |
| P1.6 | LED3 |

## 参考资料

- [XL9555 Datasheet](XL9555%20Datasheet%20Rev2.3.PDF)
- [原理图 PDF](ESP32S3电路图.pdf)
- [固件 GPIO 初始化参考](firmware_gpio_init_reference.md)
- [引脚映射验证清单](pin_mapping_verification_checklist.md)

## 关联项目

本目录的硬件设计对应上级目录中以下固件项目：

| 项目 | 主要功能 |
|------|---------|
| `test001` | XL9555 按键 / LED / 蜂鸣器基础控制 |
| `test003` | 多外设综合演示（LCD, OLED, UART, WS2812） |
| `test004` | USB Mass Storage + LCD 显示 |
| `test005` | USB U盘 + 中文 TTS 语音朗读器 |
| `test006` | BLE HID 键盘 |
| `test007` | Matter 智能灯 |
