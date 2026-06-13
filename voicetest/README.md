# Voice TTS - 多引擎文本转语音工具

支持 **讯飞** 和 **火山引擎** 两种 TTS 引擎的 Web 工具。输入文本，选择引擎和音色，即可生成语音并可在线播放和下载。

## 功能

- **双引擎**: 讯飞 TTS + 火山引擎 TTS（豆包语音合成模型 2.0）
- **多格式**: WAV / MP3 / PCM / Opus 等，按引擎自动筛选可用格式
- **多音色**: 每个引擎提供多种音色
- **在线播放**: 网页内直接播放生成的音频
- **文件下载**: 支持下载生成的音频文件
- **请求日志**: 实时显示请求历史、耗时、状态
- **输入存储**: 每次生成的文本和音频均保存到 `doc/` 目录
- **快捷键**: Ctrl+Enter 快速生成

## 项目结构

```
voice/
├── app.py                   # FastAPI 后端（路由、引擎调度、日志）
├── config.py                # API 凭证配置（用户创建）
├── config_example.py        # 配置模板
├── requirements.txt         # Python 依赖
├── audio_utils.py           # 音频格式转换（PCM↔WAV、ffmpeg 转换）
├── tts_engines/
│   ├── base.py              # TTS 引擎抽象基类
│   ├── iflytek.py           # 讯飞 TTS（WebSocket v2）
│   └── volcano.py           # 火山引擎 TTS（HTTP 单向流式 SSE）
├── static/
│   ├── index.html           # 前端页面
│   ├── style.css            # 样式
│   └── app.js               # 前端逻辑
├── doc/                     # 输入文本 + 生成音频存储目录
└── logs/                    # 日志文件
```

## 快速开始

### 1. 安装依赖

```bash
pip install -r requirements.txt
```

### 2. 配置 API 凭证

```bash
cp config_example.py config.py
```

编辑 `config.py`，填入至少一个引擎的凭证。

### 3. 启动服务

```bash
python3 app.py
```

浏览器打开 http://localhost:8000

### 4. 使用

1. 在文本框中输入要合成语音的文字
2. 选择 **TTS 引擎**（讯飞 / 火山引擎）
3. 选择 **音色**（不同引擎有不同音色库）
4. 选择 **音频格式**（自动根据引擎筛选可用格式）
5. 点击 **生成语音** 或按 Ctrl+Enter
6. 在结果区域播放或下载生成的音频

---

## 引擎配置

### 讯飞 TTS

从 [讯飞开放平台 → 语音合成](https://www.xfyun.cn/service/tts) 获取：

| 配置项 | 说明 |
|--------|------|
| `APP_ID` | 应用 ID |
| `API_KEY` | API 密钥 |
| `API_SECRET` | API 密钥 |

```python
IFLYTEK_CONFIG = {
    "APP_ID": "your_app_id",
    "API_KEY": "your_api_key",
    "API_SECRET": "your_api_secret",
}
```

**接口**: WebSocket v2（`wss://tts-api.xfyun.cn/v2/tts`）
**签名**: HMAC-SHA256（签名 host: `ws-api.xfyun.cn`）
**文本编码**: `tte: "utf8"`，文本 base64 编码后传输
**音频格式**: MP3（`aue=lame`）、WAV（PCM 转换）、PCM（`aue=raw`）

#### 音色列表

讯飞音色 ID 使用 `x4_` 前缀（对应讯飞新版发音人接口）。

| ID | 名称 | 性别 | 说明 |
|----|------|------|------|
| `x4_xiaoyan` | 小燕 | 女 | 标准女声 |
| `x4_xiaofeng` | 小峰 | 男 | 标准男声 |
| `x4_xiaowei` | 小薇 | 女 | 温柔女声 |
| `x4_xiaolin` | 小林 | 男 | 亲和男声 |
| `x4_xiaomei` | 小美 | 女 | 甜美女声 |
| `x4_xiaogang` | 小刚 | 男 | 浑厚男声 |

> 参考官方例程 `tts_ws_python3_demo/` 实现。各参数与官方 `v2/tts` WebSocket API 保持一致。

### 火山引擎 TTS（豆包语音合成模型 2.0）

从 [火山引擎控制台 → 语音合成](https://console.volcengine.com/speech/) 获取凭证。

火山引擎新版控制台与旧版控制台的鉴权方式不同，两套方式二选一即可：

| 配置项 | 适用版本 | 取值格式 |
|--------|----------|----------|
| `API_KEY` | **新版控制台**（推荐） | UUID 格式字符串，如 `423199ee-f156-411b-84d6-ff2469c54a34` |
| `ACCESS_TOKEN` | 旧版控制台（备选） | 服务端生成的 Access Token |
| `APP_ID` | 旧版控制台（备选） | 数字格式的应用 ID |
| `CLUSTER` | 旧版控制台（备选） | 默认为 `volcano_tts` |

#### 新版控制台

新版控制台只需一个 `API_KEY`（从控制台 → 语音合成 → API Key 管理 获取）。

```python
VOLCANO_CONFIG = {
    "API_KEY": "your_api_key",     # UUID 格式的 API Key
}
```

鉴权方式：WebSocket 握手时在 HTTP Header 中携带：
- `X-Api-Key`: API Key 值
- `X-Api-Resource-Id`: 资源 ID，固定为 `seed-tts-2.0`

#### 旧版控制台

旧版控制台需要 `ACCESS_TOKEN` + `APP_ID` + `CLUSTER`（从控制台 → 应用管理 获取）。

```python
VOLCANO_CONFIG = {
    "ACCESS_TOKEN": "your_access_token",
    "APP_ID": "your_app_id",
    "CLUSTER": "volcano_tts",
}
```

鉴权方式：WebSocket 握手时在 HTTP Header 中携带：
- `X-Api-App-Id`: APP ID
- `X-Api-Access-Key`: Access Token
- `X-Api-Resource-Id`: 资源 ID，固定为 `seed-tts-2.0`

> 代码优先级：`config.py` 中同时配置新版和旧版参数时，优先使用新版鉴权（`API_KEY` 不为空则走新版）。

#### 接口说明

| 项目 | 说明 |
|------|------|
| **接口地址** | `https://openspeech.bytedance.com/api/v3/tts/unidirectional`（HTTP 单向流式 SSE）|
| **请求方式** | POST，`Content-Type: application/json`，`Accept: text/event-stream` |
| **响应格式** | Server-Sent Events（SSE），每个 data 行包含 base64 编码的音频块，以 `[DONE]` 结束 |
| **模型** | `seed-tts-2.0-standard` |
| **请求体字段** | `namespace: "BidirectionalTTS"`、`req_params.speaker`（音色 ID）、`req_params.audio_params.format`（音频格式）|
| **音频格式** | WAV / MP3 / PCM / Opus（ogg_opus）|

#### 音色列表（seed-tts-2.0）

| ID | 名称 | 性别 | 说明 |
|----|------|------|------|
| `zh_female_vv_uranus_bigtts` | 薇薇 | 女 | 标准女声 |
| `zh_male_aha_mars_bigtts` | 阿浩 | 男 | 标准男声 |
| `zh_female_xiaolei_mars_bigtts` | 晓蕾 | 女 | 温柔女声 |

> 💡 可在火山引擎控制台查看完整的音色列表。如果使用旧版控制台的 Access Token，需同时配置 `ACCESS_TOKEN` + `APP_ID` + `CLUSTER`。

---

## API 文档

### `GET /api/status`

服务器状态和可用引擎信息。

### `GET /api/voices?engine=iflytek|volcano`

获取指定引擎的音色列表。

### `GET /api/formats?engine=iflytek|volcano`

获取指定引擎支持的音频格式。

### `POST /api/tts`

合成语音。

**请求体**:
```json
{
  "text": "要合成语音的文本",
  "engine": "iflytek",
  "format": "mp3",
  "voice": "x4_xiaoyan"
}
```

**参数说明**:
| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `text` | string | 是 | 合成文本，最长 5000 字符 |
| `engine` | string | 否 | `iflytek`（默认）或 `volcano` |
| `format` | string | 否 | 音频格式，根据引擎自动筛选 |
| `voice` | string | 否 | 音色 ID，不填使用引擎默认 |

**响应**:
```json
{
  "success": true,
  "file_url": "/doc/20260610_091905_3710be68.mp3",
  "filename": "20260610_091905_3710be68.mp3",
  "file_size": 22680,
  "duration_ms": 340.7,
  "request_id": "3710be68"
}
```

### `GET /api/logs?limit=50`

获取最近请求日志。

### `GET /api/doc`

列出 `doc/` 目录下的音频文件。

---

## 格式转换说明

| 引擎 | 原生格式 | 需转换格式 |
|------|----------|-----------|
| 讯飞 | MP3（`aue=lame`）、PCM（`aue=raw`） | WAV（PCM→WAV，无需 ffmpeg）|
| 火山引擎 | WAV、MP3、PCM、Opus（ogg_opus） | OGG、FLAC（需 ffmpeg）|

需要 ffmpeg 的格式会自动检测，未安装时从选项列表中隐藏。

安装 ffmpeg：
```bash
# macOS
brew install ffmpeg

# Ubuntu/Debian
apt install ffmpeg
```

## 部署到服务器

```bash
# 在服务器上
git clone <repo> /path/to/voice
cd /path/to/voice
pip install -r requirements.txt
# 编辑 config.py 填入服务器对应的 API 凭证
python3 app.py
```

如需长期运行，建议使用 systemd 或 supervisor 管理进程。

## 技术栈

- **后端**: Python 3.9+, FastAPI, Uvicorn
- **前端**: 纯 HTML/CSS/JS（无框架依赖）
- **讯飞**: WebSocket（`websocket-client`）
- **火山引擎**: HTTP SSE（`requests`）
- **音频转换**: Python `wave` 模块（WAV），可选 `ffmpeg`（MP3/OGG/FLAC/Opus）
