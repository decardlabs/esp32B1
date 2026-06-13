#!/usr/bin/env python3
"""
火山引擎智能问答 MVP - API 协议验证脚本

用途：上 ESP32 固件前，先用 Python 验证 TTS + ASR + LLM 全链路协议。
全程无需麦克风：TTS 生成语音 → ASR 识别 → LLM 回答。
用法：
  1. 确认 config.ini 已配置好
  2. python3 verify_api.py

依赖：
  pip install requests

输出：
  - test_audio.wav     — TTS 生成的测试语音
  - asr_result.txt     — ASR 识别结果
  - llm_result.txt     — LLM 完整回答
"""

import base64
import json
import os
import struct
import sys
import subprocess
import time
import uuid

import requests


# ============================================================
# 1. 读取配置
# ============================================================

def parse_ini(filepath):
    """简易 INI 解析器（与 ESP32 config_parser 逻辑一致）"""
    config = {}
    try:
        with open(filepath, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or line.startswith(";"):
                    continue
                if "=" not in line:
                    continue
                key, _, val = line.partition("=")
                key = key.strip()
                val = val.strip()
                if key:
                    config[key] = val
    except FileNotFoundError:
        print(f"[ERR] 找不到配置文件: {filepath}")
        print("      请创建配置文件或使用 -c /path/to/config.ini 指定路径")
        sys.exit(1)
    return config


def load_config():
    """加载配置，优先命令行参数，其次当前目录，最后 /sdcard/config.ini"""
    if len(sys.argv) > 2 and sys.argv[1] == "-c":
        config_path = sys.argv[2]
    elif os.path.exists("config.ini"):
        config_path = "config.ini"
    elif os.path.exists("/sdcard/config.ini"):
        config_path = "/sdcard/config.ini"
    else:
        print("[ERR] 未找到 config.ini")
        print("      请将 config.ini 放在当前目录或 /sdcard/ 下")
        print("      或用 -c /path/to/config.ini 指定路径")
        sys.exit(1)

    cfg = parse_ini(config_path)
    print(f"[OK] 读取配置文件: {config_path}")

    wifi_ssid = cfg.get("WIFI_SSID", "")
    asr_key = cfg.get("ASR_API_KEY", "")
    llm_key = cfg.get("LLM_API_KEY", "")

    if not wifi_ssid:
        print("[WARN] WIFI_SSID 未配置（本地测试不需要，ESP32 上需要）")
    if not asr_key:
        print("[ERR] ASR_API_KEY 未配置！语音识别无法工作")
        sys.exit(1)
    if not llm_key:
        print("[ERR] LLM_API_KEY 未配置！大模型无法工作")
        sys.exit(1)

    return cfg


# ============================================================
# 2. 音频工具（TTS 输出 → ASR 输入）
# ============================================================

PCM_24000_PARAMS = {"rate": 24000, "bits": 16, "channels": 1}
PCM_16000_PARAMS = {"rate": 16000, "bits": 16, "channels": 1}


def resample_24k_to_16k(input_pcm_path, output_pcm_path):
    """
    将 24000Hz 16bit mono PCM 降采样为 16000Hz。
    优先用 ffmpeg，不可用时用线性插值回退。
    """
    # 方法 A: ffmpeg
    try:
        subprocess.run(
            ["ffmpeg", "-y", "-f", "s16le", "-ar", "24000", "-ac", "1",
             "-i", input_pcm_path,
             "-f", "s16le", "-ar", "16000", "-ac", "1",
             output_pcm_path],
            capture_output=True, timeout=30)
        return True
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # 方法 B: Python 线性插值（降采样 3:2）
    with open(input_pcm_path, "rb") as f:
        raw = f.read()
    samples_24k = struct.unpack(f"<{len(raw)//2}h", raw)
    # 每 3 个 24k 样本取 2 个 → 16000Hz
    samples_16k = []
    i = 0
    while i + 3 <= len(samples_24k):
        # 简单平均每对
        s0 = samples_24k[i]
        s1 = samples_24k[i+1]
        s2 = samples_24k[i+2]
        samples_16k.append(s0)
        samples_16k.append((s1 + s2) // 2)
        i += 3
    pcm_data = struct.pack(f"<{len(samples_16k)}h", *samples_16k)
    with open(output_pcm_path, "wb") as f:
        f.write(pcm_data)
    return True


def pcm_to_wav(pcm_path, wav_path, params):
    """PCM 裸流 → WAV 文件（带 RIFF header）"""
    with open(pcm_path, "rb") as f:
        pcm_data = f.read()
    data_size = len(pcm_data)
    samplerate = params["rate"]
    channels = params["channels"]
    bits = params["bits"]
    bytes_per_sample = bits // 8

    with open(wav_path, "wb") as wf:
        # RIFF header
        wf.write(b"RIFF")
        wf.write(struct.pack("<I", 36 + data_size))
        wf.write(b"WAVE")
        # fmt chunk
        wf.write(b"fmt ")
        wf.write(struct.pack("<I", 16))  # chunk size
        wf.write(struct.pack("<H", 1))   # PCM
        wf.write(struct.pack("<H", channels))
        wf.write(struct.pack("<I", samplerate))
        wf.write(struct.pack("<I", samplerate * channels * bytes_per_sample))
        wf.write(struct.pack("<H", channels * bytes_per_sample))
        wf.write(struct.pack("<H", bits))
        # data chunk
        wf.write(b"data")
        wf.write(struct.pack("<I", data_size))
        wf.write(pcm_data)


# ============================================================
# 3. 火山引擎 TTS（生成测试语音）
# ============================================================

TTS_URL = "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
TTS_OUTPUT_WAV = "test_audio.wav"


def generate_test_audio(config, text=None):
    """
    用火山引擎 TTS 生成测试语音 → 转 16kHz WAV → 供 ASR 验证。
    这样全程无需麦克风就能跑通全链路。
    """
    print(f"\n{'='*60}")
    print(f"🔊 步骤 0: TTS 生成测试语音")
    print(f"{'='*60}")

    if not text:
        text = "你好，请问今天的天气怎么样？"

    print(f"   合成文本: \"{text}\"")

    headers = {
        "Content-Type": "application/json",
        "Accept": "text/event-stream, application/json",
        "X-Api-Key": config["ASR_API_KEY"],  # TTS 和 ASR 共用同一套 API Key
        "X-Api-Resource-Id": "seed-tts-2.0",
    }

    body = {
        "user": {"uid": "anonymous"},
        "namespace": "BidirectionalTTS",
        "req_params": {
            "text": text,
            "speaker": "zh_female_vv_uranus_bigtts",
            "model": "seed-tts-2.0-standard",
            "audio_params": {
                "format": "pcm",       # 取 PCM 裸流，方便重采样
                "sample_rate": 24000,  # TTS 固定输出 24000Hz
            },
        },
        "request_id": uuid.uuid4().hex,
    }

    try:
        resp = requests.post(
            TTS_URL, headers=headers, json=body,
            stream=True, timeout=(10, 60))
    except requests.ConnectionError:
        print(f"\n[ERR] ❌ TTS 网络连接失败")
        return None
    except Exception as e:
        print(f"\n[ERR] ❌ TTS 请求异常: {e}")
        return None

    if resp.status_code != 200:
        print(f"\n[ERR] ❌ TTS 失败 (HTTP {resp.status_code})")
        print(f"      响应: {resp.text[:200]}")
        return None

    # 解析 SSE 流，收集 PCM 数据
    pcm_chunks = []
    success = False

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
            if code is None:
                continue
            # code=0 中间帧, code=20000000 最后一帧
            if code not in (0, 20000000):
                print(f"\n[ERR] ❌ TTS 错误 (code={code}): {ev.get('message', '')}")
                return None
            data = ev.get("data")
            if data:
                pcm_chunks.append(base64.b64decode(data))
            if code == 20000000:
                success = True
                break
        except json.JSONDecodeError:
            continue

    if not success or not pcm_chunks:
        print(f"\n[ERR] ❌ TTS 未返回有效音频数据")
        return None

    # 保存 24000Hz PCM
    pcm_24k = b"".join(pcm_chunks)
    tmp_pcm = "tmp_24k.pcm"
    with open(tmp_pcm, "wb") as f:
        f.write(pcm_24k)
    print(f"   原始 PCM: 24000Hz, {len(pcm_24k)} bytes")

    # 降采样 24000 → 16000
    tmp_pcm_16k = "tmp_16k.pcm"
    print(f"   降采样 24000Hz → 16000Hz...")
    resample_24k_to_16k(tmp_pcm, tmp_pcm_16k)

    # 封装为 WAV
    pcm_to_wav(tmp_pcm_16k, TTS_OUTPUT_WAV, PCM_16000_PARAMS)

    # 清理临时文件
    for f in [tmp_pcm, tmp_pcm_16k]:
        if os.path.exists(f):
            os.remove(f)

    duration = len(pcm_24k) / (24000 * 2)  # 16bit = 2 bytes
    print(f"[OK] ✅ 测试音频已生成: {TTS_OUTPUT_WAV}")
    print(f"   音频时长: {duration:.1f}s")
    print(f"   文件格式: 16000Hz 16bit mono WAV")
    return TTS_OUTPUT_WAV


# ============================================================
# 4. 火山引擎 ASR（语音识别）
# ============================================================

# 火山引擎 ASR API 端点 — 录音文件识别（Flash 同步模式）
ASR_URL = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/recognize/flash"

# 支持的音频格式
AUDIO_FORMAT = {
    "wav": {"format": "wav", "rate": 16000, "bits": 16, "channel": 1},
    "mp3": {"format": "mp3", "rate": 16000, "bits": 16, "channel": 1},
    "pcm": {"format": "pcm", "rate": 16000, "bits": 16, "channel": 1},
    "opus": {"format": "ogg_opus", "rate": 16000, "bits": 16, "channel": 1},
}


def verify_asr(config, audio_path="test.wav"):
    """测试火山引擎 ASR 接口"""
    print(f"\n{'='*60}")
    print(f"📡 步骤 1: 测试 ASR 语音识别")
    print(f"{'='*60}")

    if not os.path.exists(audio_path):
        print(f"[WARN] 找不到测试音频文件: {audio_path}")
        print(f"       尝试自动生成静音测试音频...")
        try:
            import wave
            with wave.open(audio_path, "w") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)  # 16bit
                wf.setframerate(16000)
                wf.writeframes(b"\x00\x00" * 16000)  # 1秒静音
            print(f"[OK] 已生成静音测试音频: {audio_path}")
        except Exception as e:
            print(f"[ERR] 无法生成测试音频: {e}")
            print("      请准备一个 16kHz 16bit mono 的 WAV 文件放入脚本同目录")
            return None

    # 读取音频文件 → base64
    with open(audio_path, "rb") as f:
        audio_bytes = f.read()
    audio_b64 = base64.b64encode(audio_bytes).decode("utf-8")

    print(f"   音频文件: {audio_path}")
    print(f"   文件大小: {len(audio_bytes)} bytes, base64: {len(audio_b64)} chars")

    # 构建请求 — 豆包录音文件识别 Flash（同步模式）
    req_id = uuid.uuid4().hex
    asr_resource_id = config.get("ASR_RESOURCE_ID", "volc.seedasr.auc")

    headers = {
        "X-Api-Key": config["ASR_API_KEY"],
        "X-Api-Resource-Id": asr_resource_id,
        "X-Api-Request-Id": req_id,
        "X-Api-Sequence": "-1",
    }

    payload = {
        "user": {
            "uid": "anonymous",
        },
        "audio": {
            "data": audio_b64,
            "format": "wav",
            "rate": 16000,
            "bits": 16,
            "channel": 1,
        },
        "request": {
            "model_name": "bigmodel",
            "enable_punc": True,
            "enable_itn": True,
        },
    }

    print(f"\n   请求 URL:     {ASR_URL}")
    print(f"   请求 reqid:    {req_id}")
    print(f"   资源 ID:       {asr_resource_id}")

    t_start = time.time()

    try:
        resp = requests.post(
            ASR_URL,
            headers=headers,
            json=payload,
            timeout=(30, 60),
        )
    except requests.ConnectionError:
        print(f"\n[ERR] ❌ 网络连接失败: 无法连接到 openspeech.bytedance.com")
        print(f"      请检查网络连接或防火墙设置")
        return None
    except requests.Timeout:
        print(f"\n[ERR] ❌ ASR 请求超时 (30秒)")
        print(f"      请检查网络连接或稍后重试")
        return None
    except Exception as e:
        print(f"\n[ERR] ❌ ASR 请求异常: {e}")
        return None

    elapsed = time.time() - t_start

    # 检查响应状态码（Flash API 状态码在 response body 中）
    status_code = resp.headers.get("X-Api-Status-Code", "")
    status_msg = resp.headers.get("X-Api-Message", "")
    logid = resp.headers.get("X-Tt-Logid", "")

    print(f"   响应状态: HTTP {resp.status_code}")
    print(f"   X-Api-Status-Code: {status_code}")
    print(f"   X-Api-Message: {status_msg}")
    if logid:
        print(f"   X-Tt-Logid: {logid}")

    if resp.status_code == 401 or resp.status_code == 403:
        print(f"\n[ERR] ❌ ASR 鉴权失败 (HTTP {resp.status_code})")
        if "not granted" in status_msg:
            print(f"      API Key 未开通录音文件识别服务！请在火山引擎控制台确认：")
            print(f"      1. 确认 ASR_RESOURCE_ID 是否正确（建议：volc.seedasr.auc）")
            print(f"      2. 确认 API Key 已开通语音识别服务权限")
        else:
            print(f"      请检查 ASR_API_KEY 是否正确")
        return None
    elif resp.status_code != 200:
        print(f"\n[ERR] ❌ ASR 服务端错误 (HTTP {resp.status_code})")
        print(f"      Message: {status_msg}")
        print(f"      响应体: {resp.text[:200]}")
        return None

    # Flash 模式成功 → 解析 JSON 结果
    if status_code != "20000000":
        print(f"\n[ERR] ❌ ASR 处理失败 (code={status_code}): {status_msg}")
        print(f"      请检查音频文件格式或稍后重试")
        return None

    try:
        result = resp.json()
    except json.JSONDecodeError:
        print(f"\n[ERR] ❌ ASR 响应不是合法 JSON")
        return None

    # 提取识别文本 — Flash API 直接返回 text 字段
    text = ""
    if isinstance(result, dict):
        # 方式1: 顶层 text 字段（最直接）
        text = result.get("text", "")
        # 方式2: 从 utterances 中拼接（备选）
        if not text:
            utts = result.get("utterances", [])
            if utts:
                text = "".join(u.get("text", "") for u in utts if u.get("text"))
        # 方式3: result 字段（兜底）
        if not text:
            text = result.get("result", "")

    if isinstance(text, dict):
        text = str(text)

    print(f"\n   [OK] ✅ ASR 识别完成 (耗时 {elapsed:.1f}s)")
    print(f"   识别结果: {text or '(空)'}")

    if not text:
        print(f"   [WARN] 识别结果为空")
        return None

    # 保存结果
    with open("asr_result.txt", "w", encoding="utf-8") as f:
        f.write(text)
    print(f"   结果已保存: asr_result.txt")

    return text


# ============================================================
# 3. 火山引擎 豆包大模型 (LLM)
# ============================================================


def verify_llm(config, user_text):
    """测试火山引擎豆包大模型 LLM 接口（流式 SSE）"""
    print(f"\n{'='*60}")
    print(f"🤖 步骤 2: 测试 LLM 大模型（流式 SSE）")
    print(f"{'='*60}")

    if not user_text:
        print("[WARN] 文本为空，使用默认测试文本")
        user_text = "你好，请做自我介绍"

    # 从 config 读取 LLM 端点
    # 优先使用 LLM_ENDPOINT（完整 URL），其次 LLM_BASE_URL + /chat/completions
    llm_endpoint = config.get("LLM_ENDPOINT", "").rstrip("/")
    llm_base_url = config.get("LLM_BASE_URL", "").rstrip("/")

    if llm_endpoint:
        llm_url = llm_endpoint
    elif llm_base_url:
        llm_url = f"{llm_base_url}/chat/completions"
    else:
        print("[ERR] 未配置 LLM_ENDPOINT 或 LLM_BASE_URL！请检查 config.ini")
        return None

    llm_model = config.get("LLM_MODEL", "")
    if not llm_model:
        print("[ERR] LLM_MODEL 未配置！请检查 config.ini")
        print("    格式示例: LLM_MODEL=doubao-pro-32k")
        return None

    print(f"   端点: {llm_url}")
    print(f"   模型: {llm_model}")
    print(f"   问题: \"{user_text}\"")

    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {config['LLM_API_KEY']}",
        "Accept": "text/event-stream",  # SSE
    }

    payload = {
        "model": llm_model,
        "messages": [
            {
                "role": "system",
                "content": "你是一个智能问答助手，请简洁准确地回答用户的问题。"
            },
            {
                "role": "user",
                "content": user_text
            }
        ],
        "stream": True,   # 开启流式输出
        "max_tokens": 2048,
    }

    print(f"\n   正在连接 LLM 服务...")
    t_start = time.time()
    first_token_time = None
    full_text = ""
    chunk_count = 0

    try:
        resp = requests.post(
            llm_url,
            headers=headers,
            json=payload,
            stream=True,
            timeout=(10, 60),
        )
    except requests.ConnectionError:
        print(f"\n[ERR] ❌ 网络连接失败: 无法连接到 {llm_url}")
        print(f"      请检查网络连接或 LLM_ENDPOINT 配置")
        return None
    except requests.Timeout:
        print(f"\n[ERR] ❌ LLM 请求超时 (60秒)")
        print(f"      请检查网络连接或稍后重试")
        return None
    except Exception as e:
        print(f"\n[ERR] ❌ LLM 请求异常: {e}")
        return None

    # 检查 HTTP 状态码
    print(f"   响应状态: HTTP {resp.status_code}")

    if resp.status_code == 401:
        print(f"\n[ERR] ❌ LLM 鉴权失败 (HTTP 401)")
        print(f"      请检查 LLM_API_KEY 是否正确")
        return None
    elif resp.status_code == 404:
        print(f"\n[ERR] ❌ LLM 端点或模型不存在 (HTTP 404)")
        print(f"      请检查 LLM_ENDPOINT 和 LLM_MODEL 配置")
        return None
    elif resp.status_code == 429:
        print(f"\n[ERR] ❌ LLM 请求频率超限 (HTTP 429)")
        print(f"      请稍后重试或降低请求频率")
        return None
    elif resp.status_code != 200:
        print(f"\n[ERR] ❌ LLM 服务端错误 (HTTP {resp.status_code})")
        print(f"      响应体: {resp.text[:200]}")
        return None

    # ================================================================
    # SSE 流式解析（与 ESP32 实现逻辑一致）
    # ================================================================
    print(f"\n   开始接收 SSE 流:\n")

    try:
        for raw_line in resp.iter_lines(decode_unicode=True):
            if not raw_line:
                continue

            line = raw_line.strip()

            # 跳过注释行（SSE 规范）
            if line.startswith(":"):
                continue

            # 提取 data 内容
            if line.startswith("data: "):
                content = line[6:]
            elif line.startswith("data:"):
                content = line[5:]
            else:
                continue

            # 流结束标志
            if content == "[DONE]":
                print(f"\n   [LLM] 收到 [DONE]，流结束")
                break

            # 解析 JSON
            try:
                event = json.loads(content)
            except json.JSONDecodeError:
                print(f"\n   [WARN] 解析失败: {content[:80]}")
                continue

            # 提取 choices[0].delta.content
            choices = event.get("choices", [])
            if not choices:
                continue

            delta = choices[0].get("delta", {})
            finish_reason = choices[0].get("finish_reason")

            token_text = delta.get("content", "")
            if token_text:
                if first_token_time is None:
                    first_token_time = time.time()
                    ttfb = first_token_time - t_start
                    print(f"   [LLM] 首 token 到达耗时: {ttfb:.2f}s")
                    print(f"   ── 回答内容 ──")

                chunk_count += 1
                full_text += token_text
                # 同一行打印，模拟逐字显示效果
                print(token_text, end="", flush=True)

            if finish_reason == "stop":
                print(f"\n   ── 回答结束 ──")
                break

    except requests.ConnectionError:
        print(f"\n[ERR] ❌ SSE 流中断: 网络连接断开")
        print(f"      请检查网络连接，按 KEY3 重试")
        return None
    except Exception as e:
        print(f"\n[ERR] ❌ SSE 流解析异常: {e}")
        return None

    total_time = time.time() - t_start

    print(f"\n   接收 chunk 数: {chunk_count}")
    print(f"   总耗时: {total_time:.2f}s")
    print(f"   回答长度: {len(full_text)} 字")

    if not full_text:
        print(f"\n[WARN] LLM 返回空内容，请检查 LLM_MODEL 配置")
        return None

    # 保存结果
    with open("llm_result.txt", "w", encoding="utf-8") as f:
        f.write(full_text)
    print(f"   结果已保存: llm_result.txt")

    return full_text


# ============================================================
# 5. 主流程 — TTS → ASR → LLM 全链路验证
# ============================================================

def main():
    print("=" * 60)
    print("🌋 火山引擎智能问答 — API 全链路验证")
    print("   流程: TTS生成语音 → ASR语音识别 → LLM大模型回答")
    print("=" * 60)

    # 加载配置
    config = load_config()

    print(f"\n   配置概览:")
    print(f"     WIFI_SSID    = {config.get('WIFI_SSID', '(未配置)')}")
    print(f"     ASR_API_KEY  = {config.get('ASR_API_KEY', '(未配置)')[:8]}...")
    print(f"     LLM_API_KEY  = {config.get('LLM_API_KEY', '(未配置)')[:8]}...")
    print(f"     LLM_ENDPOINT = {config.get('LLM_ENDPOINT', '') or config.get('LLM_BASE_URL', '默认')}")
    print(f"     LLM_MODEL    = {config.get('LLM_MODEL', '(未配置)')}")

    # ---- 步骤 0: TTS 生成测试语音 ----
    tts_file = generate_test_audio(config)
    if tts_file is None:
        print(f"\n{'='*60}")
        print(f"[ERR] ❌ TTS 生成测试语音失败")
        print(f"{'='*60}")
        print(f"\n   请排查:")
        print(f"     1. ASR_API_KEY 是否也有 TTS 权限（新版控制台共用）")
        print(f"     2. 网络是否能访问 openspeech.bytedance.com")
        sys.exit(1)
    else:
        print(f"\n[OK] ✅ TTS 验证通过")

    # ---- 步骤 1: ASR 语音识别 ----
    asr_text = verify_asr(config, tts_file)

    if asr_text is None:
        print(f"\n{'='*60}")
        print(f"[ERR] ❌ ASR 验证失败")
        print(f"{'='*60}")
        print(f"\n   请排查:")
        print(f"     1. 检查 ASR_API_KEY 是否在火山引擎控制台正确获取")
        print(f"     2. 确认该 API Key 开通了语音识别服务")
        print(f"     3. 确认网络可以访问 openspeech.bytedance.com")
        print(f"     4. 降采样格式是否为 16000Hz 16bit mono")
        sys.exit(1)
    else:
        print(f"\n[OK] ✅ ASR 验证通过")

    # ---- 步骤 2: LLM 大模型回答 ----
    llm_text = verify_llm(config, asr_text)

    if llm_text is None:
        print(f"\n{'='*60}")
        print(f"[ERR] ❌ LLM 验证失败")
        print(f"{'='*60}")
        print(f"\n   请排查:")
        print(f"     1. 检查 LLM_API_KEY 是否在火山引擎方舟平台正确获取")
        print(f"     2. 确认模型 {config.get('LLM_MODEL', '')} 是否已开通")
        print(f"     3. 确认 LLM_ENDPOINT 或 LLM_BASE_URL 是否正确")
        print(f"     4. 确认网络可以访问相关域名")
        sys.exit(1)

    # ---- 全链路结果汇总 ----
    print(f"\n{'='*60}")
    print(f"🎉 全链路验证通过！")
    print(f"{'='*60}")
    print(f"")
    print(f"   全流程: TTS → ASR → LLM")
    print(f"   生成音频:  {tts_file}")
    print(f"   ASR 识别:  {asr_text}")
    print(f"   LLM 回答:  {llm_text[:100]}...")
    print(f"")
    print(f"   所有 API 协议验证无误，ESP32 固件可放心开发。")
    print(f"   将 config.ini 放入 TF 卡根目录，烧录固件即可运行。")


if __name__ == "__main__":
    main()
