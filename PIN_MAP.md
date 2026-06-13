# ESP32-S3 开发板 Pin Map

> 根据所有例程源码（`bsp_gpio.h`、`bsp_uart.h`、`bsp_i2c.c`、`bsp_spi.h`、`bsp_audio.c`、`lcd_st7796.c`、`xl9555.h`、`ws2812b.h`、`tf_sdcard.h` 等）整理。

---

## 一、ESP32-S3 直连引脚

| GPIO | 功能 / 连接外设 | 方向 | 备注 |
|------|----------------|------|------|
| GPIO0  | BOOT 按键 / WS2812B 数据输出 | 输入/输出 | 低电平进入下载模式；同时复用为 RGB LED 数据线 |
| GPIO1  | 板载 LED（蓝灯）/ LCD DC / DHT11 | 输出/双向 | 基础例程作状态 LED；高级例程（LCD）复用为 DC 信号；⚠️ 见多路复用说明 |
| GPIO2  | LCD 触摸 CS（XPT2046）| 输出 | SPI2 从机片选 |
| GPIO3  | I2S MCLK（ES8388 音频）| 输出 | I2S_NUM_0 主时钟 |
| GPIO4  | OV2640 摄像头 D0 | 输入 | DVP 数据位 0 |
| GPIO5  | OV2640 摄像头 D1 | 输入 | DVP 数据位 1 |
| GPIO6  | OV2640 摄像头 D2 | 输入 | DVP 数据位 2 |
| GPIO7  | OV2640 摄像头 D3 | 输入 | DVP 数据位 3 |
| GPIO8  | LCD 触摸笔触中断（XPT2046 IRQ）| 输入 | 低电平有效 |
| GPIO9  | I2S WS / LRCK（ES8388）| 输出 | 左右声道时钟 |
| GPIO10 | I2S DOUT → ES8388 DSDIN | 输出 | ESP32 数字音频输出 |
| GPIO11 | SPI2 MOSI | 输出 | 共用 LCD + SD 卡总线 |
| GPIO12 | SPI2 CLK  | 输出 | 共用 LCD + SD 卡总线（最高 60 MHz LCD） |
| GPIO13 | SPI2 MISO | 输入 | 共用 LCD + SD 卡总线 |
| GPIO14 | I2S DIN ← ES8388 ASDOUT | 输入 | ESP32 数字音频输入 |
| GPIO15 | OV2640 摄像头 D4 | 输入 | DVP 数据位 4 |
| GPIO16 | OV2640 摄像头 D5 | 输入 | DVP 数据位 5 |
| GPIO17 | OV2640 摄像头 D6 / UART1 TX | 输入/输出 | ⚠️ 摄像头例程作 DVP D6；UART 例程作 UART1_TX |
| GPIO18 | OV2640 摄像头 D7 / UART1 RX | 输入/输出 | ⚠️ 摄像头例程作 DVP D7；UART 例程作 UART1_RX |
| GPIO21 | LCD CS（ST7796）| 输出 | SPI2 从机片选 |
| GPIO38 | OV2640 SIOC（SCCB/I²C SCL）| 输出 | 摄像头配置总线时钟 |
| GPIO39 | OV2640 SIOD（SCCB/I²C SDA）| 双向 | 摄像头配置总线数据 |
| GPIO40 | 用户按键 KEY0（低电平有效）/ TF 卡 CS | 输入/输出 | ⚠️ 普通例程作按键输入；高级例程同时作 SD 卡片选（硬件共享） |
| GPIO41 | I²C0 SDA | 双向 | 内部上拉；挂载 XL9555 / ES8388 / SSD1306 |
| GPIO42 | I²C0 SCL | 输出 | 内部上拉 |
| GPIO45 | OV2640 PCLK | 输入 | 像素时钟 |
| GPIO46 | I2S BCLK（ES8388）| 输出 | 串行位时钟 |
| GPIO47 | OV2640 VSYNC | 输入 | 帧同步 |
| GPIO48 | OV2640 HREF  | 输入 | 行同步 |

---

## 二、SPI2 总线设备片选汇总

| 片选 GPIO | 外设 | 说明 |
|-----------|------|------|
| GPIO2  | XPT2046 触摸控制器 | 同时可触发笔触中断（GPIO8） |
| GPIO21 | ST7796 LCD 显示屏（320×480）| DC = GPIO1 |
| GPIO40 | TF/MicroSD 卡 | 与 KEY0 硬件共用，不可同时使用 |

> SPI2_HOST：MOSI=GPIO11，CLK=GPIO12，MISO=GPIO13

---

## 三、I²C0 总线设备

| I²C 地址 | 外设 | 说明 |
|----------|------|------|
| 0x10 | ES8388 音频编解码器 | 录音 + 播放，I2S 接 GPIO3/46/9/10/14 |
| 0x20 | XL9555 IO 扩展芯片 | 16 路 GPIO 扩展（A0/A1/A2 均接 GND） |
| 0x3C | SSD1306 OLED 显示屏（128×64）| 部分例程使用 |

> I2C0：SDA=GPIO41，SCL=GPIO42

---

## 四、XL9555 IO 扩展引脚（I²C 地址 0x20）

### Port 0（P0）

| 引脚 | 方向 | 功能 | 有效电平 |
|------|------|------|----------|
| P0.0 | 输出 | SPK_SD（功放使能）| 高=开启 |
| P0.1 | 输出 | BEEP（蜂鸣器）| 高=响 |
| P0.2 | 输出 | OV_PWDN（摄像头断电）| 高=断电 |
| P0.3 | 输出 | OV_RESET（摄像头复位）| 低=复位 |
| P0.4 | 输入 | KEY1 | 低=按下 |
| P0.5 | 输入 | KEY2 | 低=按下 |
| P0.6 | 输入 | KEY3 | 低=按下 |
| P0.7 | 输入 | KEY4 | 低=按下 |

### Port 1（P1）

| 引脚 | 方向 | 功能 | 有效电平 |
|------|------|------|----------|
| P1.0 | —    | 未使用 | — |
| P1.1 | 输出 | TFT_BLK（LCD 背光）| 高=亮 |
| P1.2 | 输出 | TXS0108_OE（WS2812B 电平转换使能）| 高=使能 |
| P1.3 | 输出 | TFT_RES（LCD 复位）| 低=复位 |
| P1.4 | 输出 | LED1 | 低=亮 |
| P1.5 | 输出 | LED2 | 低=亮 |
| P1.6 | 输出 | LED3 | 低=亮 |
| P1.7 | 输出 | LED4 | 低=亮 |

---

## 五、其他外设

| 外设 | 接口 | 引脚 | 备注 |
|------|------|------|------|
| WS2812B RGB LED × 4 | RMT | GPIO0 | 经 TXS0108 电平转换（由 XL9555 P1.2 控制使能） |
| DHT11 温湿度传感器 | 单总线 | GPIO1 | ⚠️ 与板载 LED 共用，需注意复用冲突 |
| LEDC PWM（SG90 舵机）| LEDC CH0 | GPIO1（LED_GPIO）| 基础 LEDC 例程使用 |
| UART1 | UART | TX=GPIO17，RX=GPIO18 | ⚠️ 与摄像头 DVP D6/D7 复用，见说明 |
| OV2640 摄像头 | DVP | 见上表 GPIO4~7，15~18，38~39，45，47，48 | XL9555 P0.2/P0.3 控制 PWDN/RESET |
| ST7796 LCD（320×480）| SPI2 | CS=GPIO21，DC=GPIO1，RST=XL9555-P1.3，BL=XL9555-P1.1 | 触摸 XPT2046：CS=GPIO2，IRQ=GPIO8 |
| MicroSD 卡 | SPI2 | CS=GPIO40 | 与 KEY0 硬件共用，不可同时使用 |
| ES8388 音频编解码器 | I²C(0x10) + I2S0 | I²C：GPIO41/42；I2S：MCLK=GPIO3，BCLK=GPIO46，LRCK=GPIO9，DOUT=GPIO10，DIN=GPIO14 | 板载麦克风 + 功放 |

---

## 六、多路复用冲突说明

| GPIO | 冲突功能 A | 冲突功能 B | 解决方案 |
|------|-----------|-----------|----------|
| GPIO0  | BOOT 按键 | WS2812B 数据 | 上电后可用作 WS2812B 数据，但需确保启动时为高电平 |
| GPIO1  | 板载 LED | LCD DC / DHT11 / LEDC | 使用 LCD 或 DHT11 例程时，板载 LED 不可独立控制 |
| GPIO17 | OV2640 D6 | UART1 TX | 摄像头与 UART 不可同时使用 |
| GPIO18 | OV2640 D7 | UART1 RX | 摄像头与 UART 不可同时使用 |
| GPIO40 | KEY0 按键 | TF 卡 CS | 读写 SD 卡期间按键状态不可靠；硬件设计共用 |

---

## 七、触摸调试后最终稳定参数（务必保持）

> 以下为当前工程实测可用配置（`/Users/macairm5/Documents/esp32/test003`），后续 AI 修改触摸逻辑时请优先保持。

### 1) XPT2046 驱动参数（`components/device/xpt2046.c`）

| 参数 | 当前值 | 说明 |
|------|--------|------|
| SPI mode | 固定 `0` | `mode 1/2` 会出现 X/Y 不更新；`0/3` 可工作，最终固定 `0` |
| `XPT2046_SWAP_XY` | `0` | 关闭轴交换，避免出现按一整条 Y 带都触发的错误映射 |
| `XPT2046_MIRROR_X` | `0` | 不镜像 |
| `XPT2046_MIRROR_Y` | `0` | 不镜像 |
| `XPT2046_RAW_X_MIN/MAX` | `220 / 3850` | 触摸校准范围 |
| `XPT2046_RAW_Y_MIN/MAX` | `260 / 3820` | 触摸校准范围 |

### 2) LVGL 调试页点击判定参数（`main/task_lcd_lvgl.c`）

| 参数 | 当前值 | 说明 |
|------|--------|------|
| `LCD_BTN_HIT_INSET_PX` | `14` | 按钮命中区域向内收缩，减少边缘误触 |

### 3) 行为约束（避免再次踩坑）

1. 不要在 `task_lcd_lvgl.c` 对触摸坐标做第二次自定义映射（容易造成左右/上下偏移叠加）。
2. 若后续需要重新校准，优先只改 `RAW_X/Y_MIN/MAX` 四个值，不要同时改 `SWAP`/`MIRROR`。
3. 若出现“Z 在变但 X/Y 不变”，先检查 SPI mode 是否被改离 `0`。
