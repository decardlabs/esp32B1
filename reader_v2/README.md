# Voice Reader - ESP32-S3 语音朗读器 (U-Disk + TTS)

基于 ESP32-S3 的 **U盘模拟 + 语音朗读器**。支持双模式：USB MSC 文件拷贝和 .txt 文件朗读。朗读支持本地 TTS（esp_tts）和云端 TTS（火山引擎），自动 fallback。

> 当前发布版本：**v2.1.0**（2026-06-13）

发布说明：`docs/RELEASE_NOTES_v2.1.0.md`

## v2.1.0 小版本更新

- 云端 TTS 新增预取流水线（prefetch ready/hit），显著降低句间等待
- 云端 HTTP 客户端启用 keep-alive 复用，减少重复建连时延
- 修复云端播放启动竞态，消除假性 `no data received`
- 音频任务栈水位日志改为低频节流，避免刷屏干扰分析
- 低内存场景下优化预取触发门槛，提高预取命中率

## 功能

- **主菜单选择模式**：开机显示三个选项（USB 拷贝 / 本地朗读 / 云端朗读）
- **USB MSC（U盘模式）**：TF 卡作为 U 盘连接到电脑
- **本地 TTS 朗读**：esp_tts（Xiaole 声音），离线可用
- **云端 TTS 朗读**：火山引擎 TTS（需 WiFi + config.ini 配置）
- **云端自动降级**：网络失败时自动回退到本地 TTS，朗读不间断
- **多编码支持**：自动识别 UTF-8 BOM、UTF-16 LE/BE
- **按键蜂鸣反馈**：XL9555 BEEP 引脚直接驱动，与音频通道独立
- **LVGL 电子书显示**：黑体 16px 中文渲染，文件名 + 文本 + 进度
- **阅读进度自动跟随**：句子滚动显示
- **初始化状态显示**：启动时逐行显示硬件初始化结果
- **大文件支持**：最大 64KB 文本文件，自动截断

## 按键映射

| 按键 | 主菜单 | USB 拷贝模式 | 阅读模式 |
|------|--------|-------------|---------|
| **KEY1** | 进入 USB 拷贝模式 | 返回主菜单 | 扫描 TF 卡 .txt 文件 |
| **KEY2** | 进入本地阅读模式 | — | 播放 / 暂停 |
| **KEY3** | 进入云端阅读模式¹ | — | 向后跳 5 句 |
| **KEY4** | — | — | 下一个文件（循环） |
| **长按 KEY1 (3s)** | — | — | 返回主菜单 |

¹ KEY3 仅在 config.ini 配置了 `WIFI_SSID` + `TTS_API_KEY` 时显示。

## 配置文件 (config.ini)

将 `config.ini` 放在 TF 卡根目录：

```ini
WIFI_SSID=你的WiFi名称
WIFI_PASS=你的WiFi密码
TTS_API_KEY=火山引擎API密钥
TTS_VOICE=zh_female_shuangkuaisisi_moon_bigtts
TTS_RESOURCE_ID=seed-tts-2.0
```

### 所有支持的配置键

| 键名 | 别名 | 说明 | 必需 |
|------|------|------|------|
| `WIFI_SSID` | — | WiFi 名称 | 云端 TTS 必需 |
| `WIFI_PASS` | — | WiFi 密码 | 云端 TTS 必需 |
| `TTS_API_KEY` | `TTS_APP_KEY`、`TTS_SECRET_ID`、`TTS_SECRET_KEY` | API 密钥（HTTP Header: `X-Api-Key`） | 云端 TTS 必需 |
| `TTS_VOICE` | — | 发音人，缺省 `zh_female_shuangkuaisisi_moon_bigtts` | 可选 |
| `TTS_RESOURCE_ID` | — | 资源 ID，缺省 `seed-tts-2.0` | 可选 |
| `TTS_APPID` | `TTS_APP_ID` | 兼容字段（当前请求路径未使用） | 可选 |
| `TTS_TOKEN` | `TTS_ACCESS_TOKEN` | 兼容字段（当前请求路径未使用） | 可选 |
| `TTS_CLUSTER` | — | 兼容字段（当前请求路径未使用） | 可选 |

> 当前固件（v2.1.0）云端请求使用 `X-Api-Key` 鉴权，因此需配置 `TTS_API_KEY`。`TTS_APPID` / `TTS_TOKEN` / `TTS_CLUSTER` 目前仅被解析保存，尚未参与请求头或请求体构造。

## 工作流程

1. **上电** → 逐行初始化 → 主菜单
2. **KEY1（USB 拷贝）** → TF 卡作为 U 盘，拷贝 .txt 文件后按 KEY1 返回
3. **KEY2 或 KEY3（本地/云端朗读）** → 进入阅读子菜单
4. **KEY1 扫描** → 按文件名排序自动加载第一个 .txt 文件
5. **KEY2 播放** → TTS/云端朗读，屏幕文字随进度滚动
6. **KEY3 跳句**（+5 句）；**KEY4 下一文件**
7. **文件读完或 KEY2** → 返回主菜单
8. **长按 KEY1 (3s)** → 任何时候返回主菜单

## 硬件依赖

- ESP32-S3 + 8MB PSRAM + 16MB Flash
- ST7796 SPI TFT (320×480)
- XL9555 I2C GPIO 扩展芯片（按键 + 背光 + 蜂鸣 + 功放控制）
- ES8388 I2S 音频编解码器
- TF/MicroSD 卡（SPI 模式）

### 引脚分配

| 功能 | GPIO | 备注 |
|------|------|------|
| SPI2 MOSI | GPIO11 | LCD + TF 卡共用 |
| SPI2 MISO | GPIO13 | |
| SPI2 CLK | GPIO12 | |
| LCD CS | GPIO21 | |
| LCD DC | GPIO1 | |
| TF 卡 CS | GPIO40 | |
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
source ~/esp/esp-idf-v5.5.1-full/esp-idf-v5.5.1/export.sh

# 编译
idf.py build

# 烧录
PORT=$(ls /dev/cu.* | rg 'usbmodem|wchusbserial|usbserial' | head -n 1)
idf.py -p "$PORT" flash

# 串口监视
idf.py -p "$PORT" monitor
```

## 项目结构

```
test009/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    ├── board_pins.h            # 引脚定义
    ├── main.c                  # 状态机 + 按键调度 + 句子拆分 + 文件管理
    ├── app_display.c/h         # LVGL 显示（黑体字库）
    ├── app_buttons.c/h         # 按键扫描与防抖
    ├── app_tts.c/h             # 本地 TTS 引擎 + 持久工作者线程
    ├── app_cloud_tts.c/h       # 云端 TTS（火山引擎 HTTP API）
    ├── app_wifi.c/h            # WiFi 连接管理
    ├── app_config.c/h          # config.ini 解析器
    ├── app_oom.c/h             # OOM 降级保护（内存不足时自动降级）
    ├── lcd_st7796.c/h          # ST7796 SPI 驱动
    ├── bsp_spi.c/h             # SPI2 总线初始化
    ├── sd_card.c/h             # TF 卡 FATFS 挂载
    ├── tusb_msc.c/h            # TinyUSB MSC 设备
    ├── i2c_bus.c/h             # I2C0 总线初始化
    ├── xl9555.c/h              # GPIO 扩展芯片
    ├── es8388.c/h              # 音频编解码器
    └── lv_font_xiaozhi_cn_16.c # 黑体 16px 2bpp 字库
```

## 内存管理

- **大文件缓冲区（128KB）存放于 PSRAM**：`s_file_content`（64KB）和 `s_file_raw`（64KB）通过 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 分配，不占用内部 SRAM
- **LVGL 显示缓冲区**：必须使用内部 DMA 内存（`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`），不支持 PSRAM 回退——DMA 不支持 PSRAM
- **TTS 引擎**：运行在专用工作者线程（栈 8KB），通过工作队列接收文本
- **内部堆监控**：poll 任务每 10 秒打印 `Free internal heap`，监测碎片化趋势

## 技术说明

**TTS 引擎**：本地使用 `espressif/esp-sr` 的 `esp_tts`（Xiaole 语音），每句结束后重建引擎防止状态累积。云端使用火山引擎 TTS API，采用 keep-alive + 持久音频任务 + 预取缓存流水线，优先命中预取缓存并并行拉取下一句。

**I2S 共享**：本地 TTS 和云端 TTS 共用 `I2S_NUM_0`。两者互斥运行：云端 TTS 先完成 HTTP 请求确认成功后再操作 I2S，不干扰本地 TTS 工作者线程。

**文本分句**：按中文标点（。！？，：；、）和 ASCII 标点分割，每句不超过 72 字节的 TTS 输入块，UTF-8 安全对齐。

**按键扫描**：20ms 间隔，60ms 防抖（3 次稳定），KEY1 支持 3 秒长按检测。按钮扫描任务在独立任务中运行，不阻塞主循环。

## 注意事项

- TF 卡需格式化为 **FAT32**，文件建议 UTF-8 编码
- USB 拷贝模式和阅读模式互斥，切换需返回主菜单
- 云端 TTS 需要 WiFi 连接可用，首次连接约 1-5 秒
- 配置文件 `config.ini` 需放在 TF 卡根目录，键值格式 `KEY=VALUE`

## 已知问题

### USB MSC 模式下串口断开
TinyUSB 初始化时会接管 USB 外设（GPIO 20/19 D+/D-），导致 USB-JTAG 串口监视器断开。如果开发板有独立 UART0 转 USB 芯片（如 CP2102），串口仍可通过 `/dev/cu.usbserial-*` 访问。返回主菜单后重新 `idf.py monitor` 即可。

### I2C XL9555 偶发初始化失败
在 Cloud TTS 初始化后直接初始化按键，XL9555 的 I2C 通信可能瞬态失败（`ESP_ERR_INVALID_STATE`）。系统会自动重试 3 次（每次间隔 20ms）。若所有重试均失败，设备仍可启动但按键不可用，需重新上电。

### 反复按 KEY1 模式切换
连续快速按 KEY1 在 USB 模式和主菜单之间切换，USB 任务（优先级 5）可能抢占按钮事件任务（优先级 2），导致 TinyUSB 内部状态机竞争。系统已通过以下机制防护：
- **USB 任务挂起**：离开 USB 模式时挂起 USB 任务（`vTaskSuspend`），进入时恢复
- **模式切换保护**：KEY1 模式切换有 250ms 防抖保护，防止双击
- **正常使用（拷文件、朗读）不受影响**

### FreeRTOS SMP Quirks
ESP-IDF v5.5.1 默认使用 SMP FreeRTOS，`StackType_t` 定义为 `uint8_t`（栈深度按 **字节** 而非字计算）。例如 `xTaskCreate(task, "name", 4096, ...)` 分配 4096 **字节**。`vTaskCoreAffinitySet()` 在非 SMP 模式下不可用，核心绑定需使用 `xTaskCreatePinnedToCore()`。

