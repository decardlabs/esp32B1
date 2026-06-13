# 火山引擎 TTS 调用说明

> 文档同步版本：v2.1.0（2026-06-13）

> 基于项目 `tts_engines/volcano.py` 整理得来，可直接用于对接火山引擎语音合成 API。

---

## 一、接口概览

| 项目 | 值 |
|------|-----|
| **接口地址** | `https://openspeech.bytedance.com/api/v3/tts/unidirectional` |
| **请求方式** | `POST` |
| **Content-Type** | `application/json` |
| **响应类型** | SSE（`text/event-stream`），Base64 编码音频块，以 `[DONE]` 结束 |
| **超时建议** | 连接 10s，读取 60s（`timeout=(10, 60)`） |

---

## 二、鉴权方式（二选一）

代码优先级：**新版优先**，即 `API_KEY` 不为空时走新版鉴权，忽略旧版参数。

### 方式 A：新版控制台（推荐）

从火山引擎控制台 → 语音合成 → API Key 管理 获取（UUID 格式）。

```http
X-Api-Key: 423199ee-f156-411b-84d6-ff2469c54a34
X-Api-Resource-Id: seed-tts-2.0
```

完整请求头示例：

```http
POST /api/v3/tts/unidirectional HTTP/1.1
Host: openspeech.bytedance.com
Content-Type: application/json
Accept: text/event-stream, application/json
X-Api-Key: <你的 API_KEY>
X-Api-Resource-Id: seed-tts-2.0
```

### 方式 B：旧版控制台

从控制台 → 应用管理 获取 `ACCESS_TOKEN`、`APP_ID`、`CLUSTER`。

```http
X-Api-App-Id: <你的 APP_ID>
X-Api-Access-Key: <你的 ACCESS_TOKEN>
X-Api-Resource-Id: seed-tts-2.0
```

> `CLUSTER` 默认值：`volcano_tts`，仅在旧版鉴权时作为参数传入请求体（实际代码中未使用，仅作配置保留）。

---

## 三、请求体（JSON）

```json
{
  "user": {
    "uid": "anonymous"
  },
  "namespace": "BidirectionalTTS",
  "req_params": {
    "text": "要合成的文本内容",
    "speaker": "zh_female_vv_uranus_bigtts",
    "model": "seed-tts-2.0-standard",
    "audio_params": {
      "format": "mp3",
      "sample_rate": 24000
    }
  },
  "request_id": "550e8400e29b41d4a716446655440000"
}
```

### 字段说明

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `user.uid` | string | ✅ | 用户标识，可填 `"anonymous"` |
| `namespace` | string | ✅ | **固定值** `"BidirectionalTTS"` |
| `req_params.text` | string | ✅ | 合成文本，建议 ≤ 5000 字符 |
| `req_params.speaker` | string | ✅ | 音色 ID，见第四节 |
| `req_params.model` | string | ✅ | **固定值** `"seed-tts-2.0-standard"` |
| `req_params.audio_params.format` | string | ✅ | 音频格式，见第五节 |
| `req_params.audio_params.sample_rate` | int | ✅ | **固定值** `24000` |
| `request_id` | string | 推荐 | 唯一请求 ID，建议用 `uuid4().hex` |

---

## 四、音色列表（seed-tts-2.0）

| speaker ID | 名称 | 性别 | 风格说明 |
|------------|------|------|----------|
| `zh_female_vv_uranus_bigtts` | 薇薇 | 女 | 标准女声（**默认音色**） |
| `zh_male_aha_mars_bigtts` | 阿浩 | 男 | 标准男声 |
| `zh_female_xiaolei_mars_bigtts` | 晓蕾 | 女 | 温柔女声 |

> 💡 音色 ID 可前往 [火山引擎控制台 → 语音合成](https://console.volcengine.com/speech/) 查看完整列表，新增音色直接替换 `speaker` 字段即可。

---

## 五、音频格式

| `audio_params.format` 值 | 说明 | 对应文件扩展名 | 需要 ffmpeg |
|--------------------------|------|---------------|-------------|
| `wav` | WAV 格式，原生支持 | `.wav` | ❌ 否 |
| `mp3` | MP3 格式，原生支持 | `.mp3` | ❌ 否 |
| `pcm` | 原始 PCM 数据 | `.pcm` | ❌ 否 |
| `ogg_opus` | Opus 编码（Ogg 容器） | `.opus` | ❌ 否 |
| `ogg` | OGG 格式 | `.ogg` | ✅ 是（需 ffmpeg 转换） |
| `flac` | FLAC 无损 | `.flac` | ✅ 是（需 ffmpeg 转换） |

> 项目中 `FORMAT_MAP` 的映射关系：
> ```python
> FORMAT_MAP = {
>     "wav": "wav",
>     "mp3": "mp3",
>     "pcm": "pcm",
>     "opus": "ogg_opus",  # 注意：调用 API 时用 ogg_opus，返回后存为 .opus
>     "ogg": "wav",        # 需 ffmpeg 将 PCM 转 OGG
>     "flac": "wav",       # 需 ffmpeg 将 PCM 转 FLAC
> }
> ```

---

## 六、SSE 响应格式解析

响应为 `text/event-stream`，每行格式如下：

```
data: {"code":0,"message":"","data":"<base64_audio_chunk>"}
data: {"code":20000000,"message":"","data":""}
[DONE]
```

### 解析规则

```python
audio_chunks = []

for raw_line in response.iter_lines(decode_unicode=True):
    if not raw_line:
        continue
    line = raw_line.strip()

    # 去掉 SSE 前缀
    if line.startswith("data:"):
        line = line[5:].strip()

    # 流结束标志
    if line == "[DONE]":
        break

    # 解析 JSON
    try:
        event = json.loads(line)
    except json.JSONDecodeError:
        continue

    # 错误判断：code 不为 0 或 20000000 即为错误
    code = event.get("code")
    if code is not None and code not in (0, 20000000):
        raise RuntimeError(f"火山引擎错误 (code={code}): {event.get('message')}")

    # 提取音频数据
    data = event.get("data")
    if isinstance(data, str) and data:
        audio_chunks.append(base64.b64decode(data))

    # code == 20000000 表示最后一帧，可提前退出
    if code == 20000000:
        break

audio_data = b"".join(audio_chunks)
```

### 结束标志（两者满足其一即可认为流结束）

1. 收到 `line == "[DONE]"`
2. 收到 `event["code"] == 20000000`

---

## 七、错误码说明

| code | 含义 |
|------|------|
| `0` | 中间音频帧，正常 |
| `20000000` | 最后一帧，合成成功结束 |
| 其他值 | 错误，需查看 `message` 字段 |

常见错误：
- `401` / 鉴权失败：检查 `X-Api-Key` 或 `X-Api-Access-Key` 是否正确
- `40001`：请求参数错误，检查 `namespace`、`speaker`、`model` 拼写
- `500`：服务端错误，建议重试

---

## 八、Python 最简调用示例

### 依赖

```bash
pip install requests
```

### 新版控制台（推荐）

```python
import base64
import json
import uuid
import requests

API_KEY = "你的_API_KEY"  # UUID 格式

headers = {
    "Content-Type": "application/json",
    "Accept": "text/event-stream, application/json",
    "X-Api-Key": API_KEY,
    "X-Api-Resource-Id": "seed-tts-2.0",
}

body = {
    "user": {"uid": "anonymous"},
    "namespace": "BidirectionalTTS",
    "req_params": {
        "text": "你好，这是火山引擎语音合成测试。",
        "speaker": "zh_female_vv_uranus_bigtts",
        "model": "seed-tts-2.0-standard",
        "audio_params": {
            "format": "mp3",
            "sample_rate": 24000,
        },
    },
    "request_id": uuid.uuid4().hex,
}

resp = requests.post(
    "https://openspeech.bytedance.com/api/v3/tts/unidirectional",
    headers=headers,
    json=body,
    stream=True,
    timeout=(10, 60),
)

resp.raise_for_status()

# 解析 SSE 流
chunks = []
for raw_line in resp.iter_lines(decode_unicode=True):
    if not raw_line:
        continue
    line = raw_line.strip()
    if line.startswith("data:"):
        line = line[5:].strip()
    if line == "[DONE]":
        break
    try:
        ev = json.loads(line)
        code = ev.get("code")
        if code is not None and code not in (0, 20000000):
            raise RuntimeError(f"错误 code={code}: {ev.get('message')}")
        data = ev.get("data")
        if data:
            chunks.append(base64.b64decode(data))
        if code == 20000000:
            break
    except json.JSONDecodeError:
        pass

with open("output.mp3", "wb") as f:
    f.write(b"".join(chunks))

print("合成完成：output.mp3")
```

### 旧版控制台

仅 Headers 不同，其余代码完全一致：

```python
headers = {
    "Content-Type": "application/json",
    "Accept": "text/event-stream, application/json",
    "X-Api-App-Id": "<你的 APP_ID>",
    "X-Api-Access-Key": "<你的 ACCESS_TOKEN>",
    "X-Api-Resource-Id": "seed-tts-2.0",
}
```

---

## 九、与项目代码的对应关系

| 说明 | 项目中的代码位置 |
|------|-----------------|
| 接口地址 | `volcano.py` 第 48 行 `self.api_url` |
| 新版鉴权 Headers | `volcano.py` 第 61–67 行 |
| 旧版鉴权 Headers | `volcano.py` 第 68–75 行 |
| 请求体构建 | `volcano.py` 第 78–91 行 |
| SSE 解析逻辑 | `volcano.py` 第 108–147 行 |
| 音色列表 | `volcano.py` 第 23–27 行 `DEFAULT_VOICES` |
| 格式映射 | `volcano.py` 第 38 行 `FORMAT_MAP` |
| 配置模板 | `config_example.py` 第 24–32 行 `VOLCANO_CONFIG` |

---

## 十、注意事项

1. **文本长度**：建议不超过 5000 字符，超长文本需自行分段调用后拼接
2. **采样率**：固定 `24000`，不支持修改（与讯飞不同）
3. **PCM 格式**：输出为 16bit 单声道 PCM，可直接用 `wave` 模块封装为 WAV
4. **ffmpeg 可选**：仅当需要使用 `ogg` / `flac` 格式时才需要安装 ffmpeg
5. **并发限制**：注意火山引擎账号的 QPS 限制，避免触发限流
6. **`[DONE]` vs `code=20000000`**：两个结束标志建议都判断，提高鲁棒性
