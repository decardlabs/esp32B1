# ESP32-S3 模拟U盘项目 (test004)

## 概述

基于 ESP32-S3 开发板，通过 TinyUSB MSC 将 TF 卡模拟为 USB 大容量存储设备，
连接电脑后显示为可读写、可格式化的 U 盘。

## 硬件配置

| 外设 | 接口 | 引脚 |
|------|------|------|
| TF 卡 | SPI2 (共享) | MOSI=11, CLK=12, MISO=13, CS=40 |
| LCD ST7796 | SPI2 (共享) | CS=21, DC=1 |
| I2C0 (XL9555 GPIO扩展) | I2C | SDA=41, SCL=42 |
| USB 口 | USB OTG | D-=19, D+=20 |
| XL9555 - LCD RST | I2C-GPIO | Port1 Pin3 |
| XL9555 - LCD BLK | I2C-GPIO | Port1 Pin1 |

## 架构

```
app_main()
│
├─ [1] I2C0 初始化 + XL9555 GPIO 扩展器初始化
│     └─ 设置 LCD 复位、背光引脚为输出
│
├─ [2] SPI2 总线初始化 + LCD 初始化
│     ├─ spi_bus_initialize(SPI2_HOST, …)
│     ├─ esp_lcd_panel_io_spi → LCD 句柄
│     ├─ 发送 ST7796 初始化序列（SLPOUT, COLMOD, DISPON…）
│     └─ 刷入静态文本帧缓存，点亮背光
│     └─ **此后 SPI2 不再被 LCD 使用**
│
├─ [3] TF 卡初始化（同一 SPI2 总线）
│     ├─ sdspi_host_init()          ← 不会重复初始化总线
│     ├─ sdspi_host_init_device()  → SPI2 上新增 SD 设备
│     └─ sdmmc_card_init()         → 获得 sdmmc_card_t*
│
└─ [4] TinyUSB MSC 设备
      ├─ tinyusb_driver_install()   ← 带 MSC 描述符
      └─ [MSC 回调]  ←→  sdmmc_read_sectors / sdmmc_write_sectors
```

## 关键设计决策

1. **非复合设备** — 仅 MSC，无 CDC。调试通过 UART0（如果板上有 USB-UART 桥）或 USB Serial/JTAG
2. **LCD 静态画面** — 初始化后不再占用 SPI2，TF 卡独占总线
3. **裸扇区访问** — 不挂载 FATFS，MSC 回调直接调用 `sdmmc_read_sectors`/`sdmmc_write_sectors`
4. **TinyUSB 原生 API** — 不依赖 `tinyusb_msc_storage` 抽象层，直接实现 MSC 回调
5. **SPI2 总线共享机制** — `sdspi_host_init()` 不初始化总线，`sdspi_host_init_device()` 在已有总线上添加设备

## USB 描述符

- 设备类: `TUSB_CLASS_MISC` (使用 IAD)
- 单个 MSC 接口，2个 Bulk 端点 (OUT=0x01, IN=0x81)
- VID=0x303A (Espressif), PID=0x4002
- 支持 USB 全速 (12Mbps)
