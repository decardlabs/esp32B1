# TTS Reader (test_tts_verify)

ESP32-S3 固件：从 TF 卡读取 `.txt` 文章，通过 TTS 语音合成朗读，配合 320×480 LCD 显示操作界面。

## 功能

- **文件浏览**：自动扫描 TF 卡上的 `.txt` 文件，按键选择
- **TTS 朗读**：中文语音合成（小乐语音），逐句朗读
- **音量调节**：5 级音量（-30 ~ 0 dB），播放中可随时调整
- **语速调节**：5 级语速，播放中可随时调整
- **暂停/继续**：随时暂停，从当前句恢复
- **编码兼容**：自动识别 UTF-8（含 BOM）、UTF-16 LE/BE

## 硬件要求

| 组件 | 说明 |
|---|---|
| SoC | ESP32-S3（16MB Flash + 8MB Octal PSRAM） |
| 显示屏 | ST7796 SPI LCD，320×480 |
| 音频解码 | ES8388（I2C 控制，I2S 音频输入） |
| GPIO 扩展 | XL9555（I2C，按键/扬声器/LCD 控制） |
| 存储 | MicroSD 卡（SPI 模式，FAT32） |
| 按键 | 4 个物理按键 |

### 引脚定义

| 外设 | 引脚 |
|---|---|
| SPI2 (LCD + SD) | MOSI=11, CLK=12, MISO=13 |
| LCD | CS=21, DC=1 |
| SD 卡 | CS=40 |
| I2C0 | SDA=41, SCL=42 |
| I2S0 | MCLK=3, BCLK=46, LRCK=9, DOUT=10 |
| XL9555 | 地址 0x20 |
| ES8388 | 地址 0x10 |

## 快速开始

### 环境准备

安装 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/)（推荐 v5.5.1），确保 `export.sh` 可用：

```bash
cd ~/esp/esp-idf
./install.sh esp32s3
```

### 构建

```bash
git clone <this-repo> test_tts_verify
cd test_tts_verify
./build.sh
```

或手动执行：

```bash
source ~/esp/esp-idf/export.sh
idf.py reconfigure
idf.py build
```

### 烧录

```bash
idf.py -p /dev/cu.usbmodem*** flash
```

### 串口监视

```bash
idf.py -p /dev/cu.usbmodem*** monitor
```

退出监视：`Ctrl+]`

## 使用方法

### 文件准备

将 `.txt` 文件（UTF-8 或 UTF-16 编码）复制到 TF 卡根目录，插入设备后重启。

### 按键操作

| 状态 | KEY1 | KEY2 | KEY3 | KEY4 |
|---|---|---|---|---|
| **文件选择** | 下一个文件 | 音量+ | 语速+ | 确认选择 |
| **就绪** | 返回文件选择 | 音量+ | 语速+ | 开始播放 |
| **播放中** | 返回文件选择 | 音量+ | 语速+ | 暂停 |
| **已暂停** | 返回文件选择 | 音量+ | 语速+ | 继续播放 |

- KEY1 长按 3 秒：功能预留

## 项目结构

```
main/
├── main.c            # 主状态机、文件管理、文本编码检测、分句
├── app_tts.c/h       # TTS 引擎（esp-sr），I2S 音频输出
├── app_display.c/h   # LVGL 显示屏界面，各状态独立画面
├── app_buttons.c/h   # 按键消抖扫描
├── sd_card.c/h       # SD 卡 FAT 文件系统
├── es8388.c/h        # ES8388 音频编解码器驱动
├── lcd_st7796.c/h    # ST7796 SPI LCD 驱动
├── bsp_spi.c/h       # SPI2 总线初始化
├── i2c_bus.c/h       # I2C0 总线初始化（带互斥锁）
├── xl9555.c/h        # XL9555 GPIO 扩展器驱动
├── lv_font_xiaozhi_cn_16.c  # 16px 中文字体
├── board_pins.h      # 所有引脚定义
├── lv_conf.h         # LVGL 配置
└── CMakeLists.txt    # 组件编译配置
```

## 设计要点

- **共享 SPI2 总线**：LCD 和 SD 卡共用 SPI2，SD 卡驱动不重复初始化总线
- **共享 I2C0 总线**：XL9555 和 ES8388 共用 I2C0，带互斥锁保护
- **TTS 工作队列**：单线程 TTS 工作任务，队列深度 4，新任务自动停止当前朗读
- **LVGL 脏标记渲染**：任何任务可设置显示状态，LVGL 任务只读渲染
- **文本编码自动检测**：通过 BOM 或零字节频率启发式识别 UTF-16

## 依赖组件

- `espressif/esp-sr` — 中文 TTS 引擎（小乐语音）
- `espressif/esp-dsp` — DSP 支持
- `espressif/cjson` — JSON 解析
- `lvgl/lvgl` — 图形库

TTS 语音数据（`esp_tts_voice_data_xiaole.dat`）编译时嵌入固件。

## 版本历史

### V1.1 (2026-06-08)

小版本发布（推荐）。

- **TTS 稳定性增强**：通过更保守的分片与工作流隔离，长文连续朗读可完整完成，未再复现此前的 parser 崩溃。
- **文本滚动修复**：修复“开始能滚动、后面不滚动”的问题，READY/PLAYING/PAUSED 文本区统一为持续自动纵向滚动。
- **排版改进**：文本区高度按整行行高对齐，避免底部出现“最后一行被截断一半”的显示问题。
- **可读性提升**：增加中文正文行间距，长文阅读更清晰。

### V3 (2026-06-07)

最终稳定版。

- **回调链式推进**：句子推进改为 TTS worker 回调直接链式触发，消除轮询竞态条件
- **去掉 TTS 引擎重建**：`esp_tts_destroy()` + `esp_tts_create()` 本身会随时间累积 NULL 指针导致崩溃，改为引擎终身留存，仅以 `esp_tts_stream_reset()` 清句子缓存
- **I2S 写入超时**：`portMAX_DELAY` 改为 1 秒超时，防止硬件卡死导致任务看门狗触发
- **Worker 栈增大**：32768 → 49152 字节，预防深度调用链栈溢出
- **堆监控**：poll 任务每 10 秒打印 `Free internal heap` + `heap_caps_check_integrity_all`

> **已知限制**：esp-sr v1.7（2022-09）中文 TTS 引擎在连续处理大量句子后存在内部内存损坏（`esp_tts_utt_append` LoadProhibited），持续朗读约 40~120 分钟后触发。系统会自动复位重启，不影响 SD 卡文件。更换朗读文件会自动重置引擎内部状态。

### V2 (2026-06-07)

中间版本（已废弃）。

- 增加 TTS 引擎定期重建（30/10/50 句间隔尝试）
- 轮询任务仅保留 5 秒安全网

### V1 (初始版本)

- 基础 TTS 朗读功能，四状态按键操作
- LVGL 显示界面
- ESP32-S3 + ES8388 + ST7796 硬件驱动
