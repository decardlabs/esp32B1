# test005 - ESP32-S3 Voice Reader (U-Disk + TTS)

基于 ESP32-S3 的 **U盘模拟 + 语音朗读器**。支持双模式主菜单：U盘拷贝文件和 TF 卡文本朗读，两种模式互斥避免文件系统冲突。

## 功能

- **主菜单选择模式**：开机后显示主菜单，用户选择 USB 拷贝或阅读模式
- **USB MSC（U盘模式）**：KEY1 进入，TF 卡作为 U 盘连接到电脑
- **阅读模式**：KEY2 进入，支持 .txt 文件扫描、朗读、翻页、进度跟随
- **按键蜂鸣反馈**：每次按键有短促蜂鸣声确认，通过 XL9555 BEEP 引脚直接驱动，不占用 I2S 音频通道
- **TFT 电子书显示**：LVGL 黑体 16px 中文显示，文件名 + 文本 + 进度
- **阅读进度自动跟随**：朗读时屏幕文字随当前句子滚动
- **多编码支持**：自动识别 UTF-8 BOM、UTF-16 LE/BE 等编码
- **初始化过程逐步显示**：启动时逐行显示各硬件初始化状态（OK/FAIL）
- **语音朗读 (TTS)**：esp_tts（xiaole 声音）→ I2S → ES8388 → 喇叭

## 按键映射

| 按键 | 主菜单 | USB 拷贝模式 | 阅读模式 |
|------|--------|-------------|---------|
| **KEY1** | 进入 USB 拷贝模式 | 返回主菜单 | 扫描 TF 卡 .txt 文件 |
| **KEY2** | 进入阅读模式 | — | 播放 / 暂停 / 完成后返回菜单 |
| **KEY3** | — | — | 向后跳 5 句 |
| **KEY4** | — | — | 下一个文件（循环） |
| **长按 KEY1** | — | — | 返回主菜单 |

## 工作流程

1. **上电** → 逐行显示初始化状态 → 主菜单
2. **选 KEY1（USB 拷贝）** → TF 卡作为 U 盘连接电脑，可拷贝 .txt 文件
3. **KEY1 返回主菜单**，再按 **KEY2（阅读模式）**
4. **按 KEY1 扫描** → 自动加载按文件名排序的第一个文件
5. **按 KEY2 播放** → TTS 朗读，屏幕文字随进度滚动
6. **KEY3** 翻页 / 快进（跳 5 句）；**KEY4** 下一文件
7. **文件读完** → 显示完成提示，KEY4 下一文件，KEY2 返回主菜单
8. **长按 KEY1** 任何时候返回主菜单切换模式

## 硬件依赖

- ESP32-S3 + 8MB PSRAM + 16MB Flash
- ST7796 SPI TFT (320×480)
- XL9555 I2C GPIO 扩展芯片
- **ES8388 I2S 音频编解码器**（板载功放+喇叭）
- **TF/MicroSD 卡**（SPI 模式，CS=GPIO40）
- KEY1-KEY4（通过 XL9555 P0.4-P0.7 读取，低电平有效）

### 引脚分配

| 功能 | GPIO | 备注 |
|------|------|------|
| SPI2 MOSI | GPIO11 | LCD + TF 卡共用 |
| SPI2 MISO | GPIO13 | |
| SPI2 CLK | GPIO12 | |
| LCD CS | GPIO21 | |
| LCD DC | GPIO1 | |
| TF 卡 CS | GPIO40 | 与 KEY0 共用脚 |
| I2C0 SDA | GPIO41 | XL9555 + ES8388 |
| I2C0 SCL | GPIO42 | |
| SPK_SD | XL9555 P0.0 | 功放使能，低电平开启 |
| I2S0 MCLK | GPIO3 | |
| I2S0 BCLK | GPIO46 | |
| I2S0 LRCK | GPIO9 | |
| I2S0 DOUT | GPIO10 | |

## 编译与烧录

```bash
# 设置 ESP-IDF 环境
source ~/esp/esp-idf-v5.5.1/export.sh

# 进入项目目录
cd /Users/macairm5/Documents/esp32/test005

# 获取 LVGL 等依赖
idf.py reconfigure

# 编译
idf.py build

# 烧录（使用 usbmodem 端口）
PORT=$(ls /dev/cu.* | rg 'usbmodem' | head -n 1)
idf.py -p "$PORT" flash

# 如烧录不稳定，降低波特率重试：
python -m esptool --chip esp32s3 -p "$PORT" -b 115200 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m --no-compress 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/voice_reader.bin
```

## 项目结构

```
test005/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    ├── board_pins.h          # 引脚定义
    ├── main.c                # 主入口 + 主菜单 + 按键调度 + 句子拆分
    ├── app_display.c/h       # LVGL 显示（黑体字库）
    ├── app_buttons.c/h       # 按键扫描与防抖
    ├── app_tts.c/h           # TTS 持久工作者线程 + I2S 输出 + 蜂鸣
    ├── lcd_st7796.c/h        # ST7796 LCD 驱动（esp_lcd 框架 + DMA）
    ├── bsp_spi.c/h           # SPI 总线初始化 + 总线锁
    ├── sd_card.c/h           # TF 卡 FAT 挂载
    ├── tusb_msc.c/h          # USB MSC（U盘，仅 MSC 无 CDC）
    ├── i2c_bus.c/h           # I2C 总线初始化
    ├── xl9555.c/h            # GPIO 扩展芯片 + 按键 + 喇叭控制
    ├── es8388.c/h            # 音频编解码器驱动
    └── lv_font_xiaozhi_cn_16.c  # 黑体 16px 字库（2bpp）
```

## 技术说明

**TTS 引擎**：`espressif/esp-sr` 的 `esp_tts` 引擎，语音为 **xiaole**，语音数据嵌入固件。
采样率 16kHz，单声道，I2S 飞利浦格式输出到 ES8388。语速 speed=2（范围 0~5）。

**按键反馈**：所有按键按下触发 35ms 蜂鸣声，通过 XL9555 BEEP 引脚直接驱动，
不经过 I2S/I2C，与 TTS 播放互不干扰，响应迅速。

**I2S DMA 配置**：`dma_desc_num=8`, `dma_frame_num=256`, `auto_clear=true`。
参考 test100 项目的配置以提升播放稳定性。

**持久工作者线程**：TTS 引擎运行在单一持久任务中，通过工作队列接收文本，
避免反复创建/销毁任务导致的内存碎片和竞态条件。

**阅读进度跟随**：每播完一句，屏幕文字自动滚动到当前句子起始位置。

**GPIO46（BCLK）限制**：ESP32-S3 的 GPIO46 为 RTC 域引脚，驱动能力弱，
用作 I2S BCLK 输出时时钟信号质量有限，可能导致音频底噪。这是硬件设计限制。

## 注意事项

- TF 卡需格式化为 **FAT32**，文件建议使用 UTF-8 编码
- USB 拷贝模式和阅读模式互斥，切换需返回主菜单
- 如果烧录不稳定，尝试更换 USB 口或降低波特率
- 两个 USB 口：一个用于烧录/日志（usbmodem），另一个用于 U 盘功能（仅 MSC）

## 版本历史

### V2.0 (2026-06-08) — 稳定性重构

从 test_tts_verify 对齐全部稳定性优化。

**TTS 引擎改进（app_tts.c）**
- **文本清洗**：新增 `sanitize_tts_text()`，过滤引号/括号/特殊标点，限制可朗读字符上限（18个），防止 TTS 解析器遇到边缘字符时触发内部堆损坏
- **微分片解析**：每片最多 4 个字符（`TTS_PARSE_FRAGMENT_CHARS=4`），独立 reset→parse→play→reset，避免 `utt_extend` 缓冲区过度增长触发的 `realloc/free` 指针损坏
- **逐句引擎重建**：每次 utterance 后销毁并重建 TTS 引擎（`TTS_RECREATE_INTERVAL=1`），复用已有 voice 句柄（不重新 `voice_set_init`），内部状态永不累积
- **任务看门狗管理**：在阻塞式 `esp_tts_parse_chinese` 前后挂起/恢复 WDT 监控，防止假阳性 WDT 触发
- **Worker 优先级从 3 降到 2**，降低对 LVGL 渲染的抢占

**系统配置对齐**
- PSRAM 速度 80MHz → **40MHz**（消除高频堆数据错误）
- INT WDT 超时 300ms → **1200ms**（防止 SPI 总线锁竞争触发 TG1WDT）
- Task WDT 超时 5s → **15s**（给长句 TTS 解析留余量）
- LVGL 任务栈 6144 → **8192**（安全余量）
- esp-sr 组件升级 v1.7 → **v2.4.6**

**应用层修复（main.c）**
- 回调链式推进（消除轮询竞态条件）
- 分句限长 72 字节，UTF-8 安全对齐
- 堆健康监控（每 10 秒打印 free heap + 完整性检查）
- 重置 `s_file_loaded` 在所有加载失败路径
- `do_skip`/`do_play`/`do_pause` 增加 chunk 偏移重置
- Poll 任务仅保留 5 秒安全网，正常推进由回调驱动
- `sd_card.c` 移除已废弃的 `host.init_sdspi`

### V1.0 — 初始版本

初始开发版本，USB MSC + TTS 朗读基础功能。
