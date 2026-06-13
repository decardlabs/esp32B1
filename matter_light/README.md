# matter_light — ESP-Matter On/Off Light

基于 ESP32-S3 的 Matter 智能灯，支持 ST7796 TFT 触摸屏显示、LVGL 图形界面、WS2812B 灯带控制。

## 硬件

- **主控**: ESP32-S3 (rev v0.2), Xtensa LX7 双核 240MHz
- **PSRAM**: 8MB Octal PSRAM (80MHz)
- **Flash**: 4MB SPI Flash (DIO 80MHz)
- **显示屏**: ST7796 SPI TFT, 320×480, 驱动 IC ST7796
- **触摸**: FT6336 (I2C), CS=GPIO2, IRQ=GPIO8
- **LED**: WS2812B 灯带, GPIO0
- **按键**: 
  - BOOT (GPIO 0) — 共用 WS2812 数据脚
  - KEY0 (GPIO 40) — 长按 3 秒恢复出厂

### GPIO 分配

| 功能 | GPIO | 备注 |
|---|---|---|
| WS2812 / BOOT 键 | 0 | 启动时为输入，初始化后为 WS2812 输出 |
| LCD DC / 状态 LED | 1 | 共用脚 |
| 触摸 CS | 2 | |
| 触摸 IRQ | 8 | |
| SPI2 MOSI | 11 | LCD 数据 |
| SPI2 CLK | 12 | LCD 时钟 |
| SPI2 MISO | 13 | |
| UART1 TX / CAM D6 | 17 | |
| UART1 RX / CAM D7 | 18 | |
| LCD CS | 21 | |
| KEY0 / TF SD CS | 40 | 长按 3s 恢复出厂 |
| I2C0 SDA | 41 | XL9555 等 |
| I2C0 SCL | 42 | |

### 外设

- **XL9555** — I2C IO 扩展芯片，控制 WS2812 电平转换器使能
- **ST7796** — SPI 接口 TFT 显示屏
- **WS2812B** — 智能 LED 灯带
- **FT6336** — 电容触摸屏控制器

## 软件

- **ESP-IDF**: v5.5.1
- **ESP-Matter**: latest (connectedhomeip)
- **LVGL**: v8.3.11 — 图形界面
- **NimBLE** — BLE 协议栈 (Matter 配网)
- **FreeRTOS** — 实时操作系统

### 项目结构

```
├── CMakeLists.txt          # 项目构建文件
├── partitions.csv          # 分区表 (2个OTA分区)
├── sdkconfig               # 完整配置
├── sdkconfig.defaults      # 默认配置
├── main/
│   ├── main.c              # 入口 + KEY0 恢复出厂任务
│   ├── app_matter.cpp      # Matter 协议栈初始化 + 配网回调
│   ├── app_matter.h
│   ├── app_driver.c        # 灯控制驱动
│   ├── app_driver.h
│   ├── task_lcd_lvgl.c     # LVGL 显示任务 + UI 布局
│   ├── task_lcd_lvgl.h
│   ├── qrcode_utils.c      # QR 码生成 (集成 LVGL 图像)
│   ├── qrcode_utils.h
│   ├── qrcodegen.c/h       # QR Code 编码库
│   ├── Kconfig.projbuild
│   └── include/
├── components/
│   ├── bsp/                # 板级支持包 (I2C, SPI, GPIO)
│   └── device/             # 设备驱动
│       ├── lcd_st7796.c/h  # LCD 驱动
│       ├── ws2812b.c/h     # LED 灯带驱动
│       └── xl9555.c/h      # IO 扩展芯片
├── managed_components/     # ESP 组件管理器依赖
│   ├── lvgl__lvgl/
│   └── espressif__*/
└── build/                  # 编译输出
```

## 快速开始

### 环境要求

- ESP-IDF v5.5.1
- ESP-Matter
- Python 3.14+, CMake 4.3+
- xtensa-esp-elf-gcc 14.2.0 工具链

### 设置环境

```bash
source ~/esp/esp-idf/export.sh
```

### 编译

```bash
idf.py build
```

### 烧录

首次烧录（包含分区表）：
```bash
idf.py -p /dev/cu.usbmodem101 erase-flash
idf.py -p /dev/cu.usbmodem101 flash
```

后续更新：
```bash
idf.py -p /dev/cu.usbmodem101 flash
```

### 串口监控

```bash
idf.py -p /dev/cu.usbmodem101 monitor
```

## 功能

### 配网流程

1. 首次启动 → 屏幕显示 **二维码 + 配对码**
2. 打开 iPhone Home → 扫码添加
3. 通过 BLE 接收 Wi-Fi 凭证 → 连接 → 配网完成
4. 屏幕自动切换到运行界面

### 运行界面

```
  ESP Matter Light      ← 标题
     Light: OFF         ← 居中，大字；开灯时变绿色 "Light: ON"
  ─────────────────
  Wi-Fi Connected       ← 网络状态
  Initializing... / Ready  ← 10 秒后变为 Ready
```

### 状态指示

| 阶段 | 屏幕中部 | 底部状态 |
|---|---|---|
| 初始化 | Light: OFF | **Initializing...** |
| Wi-Fi 就绪 | Light: OFF | **Initializing...** |
| 完全就绪（~10s） | Light: OFF | **Ready** |
| 手机开关灯 | **Light: ON** 🟢 / Light: OFF | Ready |
| 首次配网 | 二维码 + 配对码 | Waiting for pairing... |

### 恢复出厂设置

**KEY0 (GPIO 40) 长按 3 秒** → 自动擦除 NVS → 重启 → 显示二维码可重新配对

## 关键配置

### 内存

| 参数 | 值 | 说明 |
|---|---|---|
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE` | 16384 | main 任务栈 |
| `CONFIG_CHIP_TASK_STACK_SIZE` | 16384 | Matter 任务栈 |
| `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` | 131072 | DMA 保留池 (128KB) |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` | 2048 | malloc < 2KB 优先内部 RAM |

### PSRAM

- Octal PSRAM 8MB @ 80MHz
- `SPIRAM_USE_MALLOC` — malloc 自动利用 PSRAM
- `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` — WiFi/LWIP 缓冲区优先使用 PSRAM

### 分区表

| 分区 | 偏移 | 大小 |
|---|---|---|
| nvs | 0x10000 | 48KB |
| nvs_keys | — | 4KB (加密) |
| otadata | 0x1D000 | 8KB |
| phy_init | 0x1F000 | 4KB |
| ota_0 | 0x20000 | 1920KB |
| ota_1 | 0x200000 | 1920KB |

## 开发说明

### 添加新功能

- 添加 endpoint: 修改 `app_matter.cpp` 中 `app_matter_start()` 里的 endpoint 配置
- 添加 UI 元素: 修改 `task_lcd_lvgl.c` 中的 `lcd_lvgl_show_operational()` 或 `lcd_lvgl_show_pairing_info()`
- 添加 GPIO: 在 `components/bsp/include/bsp_board_pins.h` 定义

### 已知限制

- BOOT 键 (GPIO 0) 与 WS2812 共用引脚，运行时不可作为按键使用
- QR 码图像在 PSRAM 中分配，LVGL 渲染正常
- Matter 配网超时后需重新启动设备进入配网模式
