# test003 — ESP32-S3 多外设综合演示

基于 ESP-IDF 的 ESP32-S3 综合外设演示项目，通过 **编译配置（menuconfig）** 选择不同功能组合。单份代码覆盖 LCD/LVGL 显示、OLED 传感器显示、WS2812 灯带、外部中断等多个场景。

## 功能配置（App Profile）

通过 `idf.py menuconfig → App Profile` 选择以下四种模式之一：

| Profile | 主要功能 |
|---------|---------|
| `LCD_UI` | ST7796 TFT + LVGL 图形界面（默认） |
| `SENSOR_OLED` | DHT11 温湿度传感器 + SSD1306 OLED 显示 |
| `WS2812` | WS2812B 全彩 LED 灯带效果 |
| `EXTI_LED` | 外部中断触发 LED 响应 |

各 Profile 均可叠加以下可选功能（通过 menuconfig 独立开关）：

- UART1 透传
- CAMERA（OV2640 摄像头）
- TF/SD 卡

## 主要外设与驱动

| 外设 | 驱动文件 | 说明 |
|------|---------|------|
| XL9555 I2C GPIO 扩展 | `xl9555.c/h` | 按键、LCD 背光/复位、WS2812 使能 |
| ST7796 SPI TFT | `lcd_st7796.c/h` | 320×480 彩色显示屏 |
| LVGL | `task_lcd_lvgl.c/h` | 图形界面框架（v8.x） |
| SSD1306 OLED | `task_oled.c/h` | 128×64 I2C OLED |
| DHT11 | `task_dht11.c/h` | 温湿度传感器 |
| WS2812B | `task_ws2812.c/h` | 可寻址 RGB LED 灯带 |
| LEDC | `task_ledc.c/h` | PWM 调光 |
| UART | `task_uart.c/h` | UART1 通信 |
| GPTimer | `bsp_gptimer.c/h` | 高精度定时器（LED 闪烁） |

## 引脚分配（关键信号）

| 功能 | GPIO | 备注 |
|------|------|------|
| I2C0 SDA | GPIO41 | XL9555 + ES8388 共用 |
| I2C0 SCL | GPIO42 | |
| SPI2 MOSI | GPIO11 | LCD + TF 卡共用 |
| SPI2 MISO | GPIO13 | |
| SPI2 CLK | GPIO12 | |
| LCD CS | GPIO21 | |
| LCD DC | GPIO1 | 与 DHT11 共用（不同 Profile） |
| KEY0 / TF CS | GPIO40 | 共用脚，Profile 间互斥 |

> 引脚冲突通过编译期 `#error` 静态检查，选错 Profile 时构建即报错。

## 项目结构

```
test003/
├── CMakeLists.txt
├── components/              # 板级支持包（BSP）
│   ├── bsp/                 # I2C、SPI、GPIO、UART、LEDC、GPTimer
│   └── device/              # XL9555、ST7796、SSD1306 驱动
├── managed_components/      # LVGL 等 ESP 组件管理器依赖
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild    # menuconfig 选项定义
│   ├── main.c               # 入口 + Profile 选择逻辑
│   ├── task_lcd_lvgl.c/h    # LCD + LVGL 任务
│   ├── task_oled.c/h        # OLED 显示任务
│   ├── task_dht11.c/h       # DHT11 读取任务
│   ├── task_ws2812.c/h      # WS2812B 灯带任务
│   ├── task_led.c/h         # LED 闪烁任务
│   ├── task_ledc.c/h        # LEDC PWM 任务
│   ├── task_uart.c/h        # UART 通信任务
│   ├── task_exti_prco.c/h   # 外部中断任务
│   └── task_timer.c/h       # 定时器任务
└── docs/                    # 设计文档（目录保留）
```

## 编译与烧录

```bash
# 设置 ESP-IDF 环境（v5.5.1）
source ~/esp/esp-idf-v5.5.1/export.sh

# 进入项目目录
cd /Users/macairm5/Documents/esp32/test003

# 选择功能 Profile（首次必须执行）
idf.py menuconfig
# → App Profile → 选择 LCD_UI / SENSOR_OLED / WS2812 / EXTI_LED

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
- **LVGL**: v8.x（通过 ESP 组件管理器引入）
- **依赖**: `espressif/ssd1306`、`idf::dht`
