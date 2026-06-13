# test001 — ESP32-S3 XL9555 按键 / LED / 蜂鸣器控制

基于 ESP-IDF 的最小化 ESP32-S3 示例，通过 **XL9555 I2C GPIO 扩展芯片**驱动 3 路按键、3 路 LED 和蜂鸣器。

## 功能

- **轮询 3 路按键**（XL9555），带 30ms 软件消抖
- **短按**：KEY1 / KEY2 / KEY3 分别切换对应 LED 状态
- **KEY2 短按**额外触发蜂鸣器鸣叫 600ms
- **长按（≥ 1s）**：对应 LED 进入 1Hz 闪烁模式，松开后恢复

## 硬件引脚

| 信号 | XL9555 引脚 |
|------|------------|
| KEY1 | P0.4 |
| KEY2 | P0.5 |
| KEY3 | P0.6 |
| LED1 | P1.4 |
| LED2 | P1.5 |
| LED3 | P1.6 |
| BEEP | P0.1 |

I2C 总线配置（`main/xl9555_io.c`）：

| 参数 | 值 |
|------|---|
| I2C 端口 | I2C_NUM_0 |
| SCL | GPIO42 |
| SDA | GPIO41 |
| 地址 | 0x20 |

## 项目结构

```
test001/
├── CMakeLists.txt
└── main/
    ├── CMakeLists.txt
    ├── main.c              # 入口，创建 key-led 任务
    ├── task_key_led.c      # 按键扫描 / LED / 蜂鸣逻辑
    ├── xl9555_io.c         # XL9555 I2C 读写封装
    └── include/
        ├── io_map.h        # 引脚映射定义
        ├── task_key_led.h
        └── xl9555_io.h
```

## 编译与烧录

```bash
# 设置 ESP-IDF 环境（v5.5.1）
source ~/esp/esp-idf-v5.5.1/export.sh

# 设置目标芯片
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录并监控
PORT=$(ls /dev/cu.* | rg 'usbmodem|wchusbserial|usbserial' | head -n 1)
idf.py -p "$PORT" flash monitor
```

## 软件环境

- **ESP-IDF**: v5.5.1
- **芯片**: ESP32-S3
- **依赖**: 无外部组件依赖
