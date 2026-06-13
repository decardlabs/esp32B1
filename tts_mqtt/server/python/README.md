# python-vol

这个目录就是我现在在用的火山引擎 TTS Python bridge。

它的职责比较单一：

- 接收 PHP 发过来的 TTS 预生成请求
- 去请求火山引擎的流式 TTS
- 对外提供 ESP32 能直接访问的 `stream_url`

当前它所在的位置是：

```text
网页
-> PHP
-> python-vol
-> Volcengine TTS
-> MQTT
-> ESP32
-> HTTP 实时拉流播放
```

## 目录说明

- `tts_bridge_server.py`
  服务入口
- `tts_bridge_config.py`
  当前配置文件
- `requirements.txt`
  依赖列表

## 安装依赖

```bash
cd /你的部署目录/python-vol
python3 -m pip install -r requirements.txt
```

## 配置

我现在把配置放在：

```text
python-vol/tts_bridge_config.py
```

这里需要按我自己的环境去填：

- `VOLCENGINE_API_KEY`
- `VOLCENGINE_RESOURCE_ID`
- `VOLCENGINE_SPEAKER`
- `TTS_PUBLIC_BASE_URL`

这几个值分别对应：

- 火山引擎密钥
- 资源 ID
- 音色
- ESP32 最终能访问到的 Python 服务地址

## 启动

```bash
cd /你的部署目录/python-vol
python3 tts_bridge_server.py --host 0.0.0.0 --port 9100
```

## 健康检查

```bash
curl http://127.0.0.1:9100/health
```

正常情况下会返回：

```json
{
  "ok": true,
  "service": "volcengine-tts-bridge"
}
```

## 主要接口

- `POST /api/tts`
- `GET /tts/{request_id}.mp3`
- `GET /api/tts/{request_id}`

## 它和 PHP 的关系

PHP 默认会通过本机地址来调这套服务：

```php
'python_tts' => [
    'prepare_url' => 'http://127.0.0.1:9100/api/tts',
    'timeout' => 20,
],
```

所以我现在默认的部署方式是：

- PHP 和 `python-vol` 放在同一台服务器
- PHP 负责分段、预生成、顺序下发
- `python-vol` 负责对接火山引擎并生成可播放流地址
