#!/usr/bin/env python3
"""
火山引擎智能问答 — 全链路 Demo

流程：TTS生成语音 -> ASR语音识别 -> LLM大模型回答 -> TTS语音合成
跑完这一遍，相当于完整验证了语音问答的每一个环节。

用法：
  python3 demo_qa_loop.py

依赖：
  pip install requests

输出：
  output/
    01_question.wav       -- 问题1的TTS语音
    01_answer.wav         -- 回答1的TTS语音
    02_question.wav
    02_answer.wav
    ...
    transcript.txt        -- 完整问答记录
"""

import base64
import json
import os
import struct
import subprocess
import sys
import time
import uuid

import requests


# ============================================================
# 配置加载
# ============================================================

def parse_ini(filepath):
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
        print("[ERR] 找不到配置文件:", filepath)
        sys.exit(1)
    return config


def load_config():
    if len(sys.argv) > 2 and sys.argv[1] == "-c":
        config_path = sys.argv[2]
    elif os.path.exists("config.ini"):
        config_path = "config.ini"
    else:
        print("[ERR] 未找到 config.ini")
        sys.exit(1)
    cfg = parse_ini(config_path)
    return cfg


# ============================================================
# 音频工具
# ============================================================

def resample_24k_to_16k(input_pcm, output_pcm):
    """24000Hz -> 16000Hz PCM 降采样"""
    try:
        subprocess.run(
            ["ffmpeg", "-y", "-f", "s16le", "-ar", "24000", "-ac", "1",
             "-i", input_pcm,
             "-f", "s16le", "-ar", "16000", "-ac", "1",
             output_pcm],
            capture_output=True, timeout=30)
        return True
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    # 回退：线性插值 3:2
    with open(input_pcm, "rb") as f:
        raw = f.read()
    samples = [s for s in struct.iter_unpack("<h", raw)]
    out = []
    for i in range(0, len(samples) - 2, 3):
        out.append(samples[i][0])
        out.append((samples[i+1][0] + samples[i+2][0]) // 2)
    with open(output_pcm, "wb") as f:
        for s in out:
            f.write(struct.pack("<h", s))
    return True


def pcm_to_wav(pcm_path, wav_path, rate=16000):
    """PCM -> WAV"""
    with open(pcm_path, "rb") as f:
        pcm_data = f.read()
    data_size = len(pcm_data)
    with open(wav_path, "wb") as wf:
        wf.write(b"RIFF")
        wf.write(struct.pack("<I", 36 + data_size))
        wf.write(b"WAVE")
        wf.write(b"fmt ")
        wf.write(struct.pack("<I", 16))
        wf.write(struct.pack("<H", 1))
        wf.write(struct.pack("<H", 1))
        wf.write(struct.pack("<I", rate))
        wf.write(struct.pack("<I", rate * 2))
        wf.write(struct.pack("<H", 2))
        wf.write(struct.pack("<H", 16))
        wf.write(b"data")
        wf.write(struct.pack("<I", data_size))
        wf.write(pcm_data)


# ============================================================
# 1. TTS: 文字 -> 语音
# ============================================================

def tts_speak(config, text, output_wav):
    """火山引擎 TTS：文字 -> 16000Hz WAV"""
    headers = {
        "Content-Type": "application/json",
        "Accept": "text/event-stream, application/json",
        "X-Api-Key": config["ASR_API_KEY"],
        "X-Api-Resource-Id": "seed-tts-2.0",
    }
    body = {
        "user": {"uid": "anonymous"},
        "namespace": "BidirectionalTTS",
        "req_params": {
            "text": text,
            "speaker": "zh_female_vv_uranus_bigtts",
            "model": "seed-tts-2.0-standard",
            "audio_params": {"format": "pcm", "sample_rate": 24000},
        },
        "request_id": uuid.uuid4().hex,
    }

    try:
        resp = requests.post(
            "https://openspeech.bytedance.com/api/v3/tts/unidirectional",
            headers=headers, json=body, stream=True, timeout=(10, 60))
    except Exception as e:
        print("      [ERR] TTS 请求失败:", e)
        return False

    if resp.status_code != 200:
        print("      [ERR] TTS HTTP", resp.status_code)
        return False

    chunks, ok = [], False
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
            if code not in (0, 20000000):
                return False
            data = ev.get("data")
            if data:
                chunks.append(base64.b64decode(data))
            if code == 20000000:
                ok = True
                break
        except json.JSONDecodeError:
            continue

    if not ok or not chunks:
        return False

    pcm_24k = b"".join(chunks)
    tmp24 = output_wav + ".tmp24.pcm"
    tmp16 = output_wav + ".tmp16.pcm"
    with open(tmp24, "wb") as f:
        f.write(pcm_24k)
    resample_24k_to_16k(tmp24, tmp16)
    pcm_to_wav(tmp16, output_wav, 16000)
    for f in [tmp24, tmp16]:
        if os.path.exists(f):
            os.remove(f)
    return True


# ============================================================
# 2. ASR: 语音 -> 文字
# ============================================================

def asr_recognize(config, wav_path):
    """火山引擎 Flash ASR：语音 -> 文字"""
    resource_id = config.get("ASR_RESOURCE_ID", "volc.seedasr.auc")
    try:
        with open(wav_path, "rb") as f:
            audio_b64 = base64.b64encode(f.read()).decode("utf-8")
    except Exception as e:
        print("  [ASR] 读取文件失败:", e)
        return None

    req_id = uuid.uuid4().hex
    headers = {
        "X-Api-Key": config["ASR_API_KEY"],
        "X-Api-Resource-Id": resource_id,
        "X-Api-Request-Id": req_id,
        "X-Api-Sequence": "-1",
    }
    payload = {
        "user": {"uid": "anonymous"},
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

    try:
        resp = requests.post(
            "https://openspeech.bytedance.com/api/v3/auc/bigmodel/recognize/flash",
            headers=headers, json=payload, timeout=(30, 60))
    except Exception as e:
        print("  [ASR] 请求失败:", e)
        return None

    sc = resp.headers.get("X-Api-Status-Code", "")
    msg = resp.headers.get("X-Api-Message", "")
    if resp.status_code != 200:
        print("  [ASR] HTTP", resp.status_code, ":", resp.text[:100])
        return None
    if sc != "20000000":
        print("  [ASR] 处理失败 code=", sc, ":", msg)
        return None

    result = resp.json()
    if isinstance(result, dict):
        def _extract(d):
            if isinstance(d, str):
                return d
            if isinstance(d, dict):
                t = d.get("text", "")
                if t:
                    return t
                for v in d.values():
                    r = _extract(v)
                    if r:
                        return r
            return ""
        text = _extract(result)
        if text and isinstance(text, str):
            return text
        print("  [ASR] 响应结构异常:", str(result)[:120])
        return None


# ============================================================
# 3. LLM: 文字 -> 回答 (Responses API)
# ============================================================

def llm_chat(config, question):
    """火山引擎豆包 LLM（Responses API 流式 SSE）：问题 -> 回答"""
    endpoint = config.get("LLM_ENDPOINT", "").rstrip("/")
    model = config.get("LLM_MODEL", "")
    if not endpoint:
        print("  [LLM] LLM_ENDPOINT 未配置")
        return None
    if not model:
        print("  [LLM] LLM_MODEL 未配置")
        return None

    print("\n         端点:", endpoint)
    print("         模型:", model)

    headers = {
        "Content-Type": "application/json",
        "Authorization": "Bearer " + config["LLM_API_KEY"],
    }

    payload = {
        "model": model,
        "stream": True,
        "input": [
            {
                "role": "user",
                "content": [
                    {
                        "type": "input_text",
                        "text": question,
                    }
                ],
            }
        ],
    }

    try:
        resp = requests.post(endpoint, headers=headers, json=payload,
                             stream=True, timeout=(10, 60))
    except Exception as e:
        print("  [LLM] 请求失败:", e)
        return None

    if resp.status_code == 404:
        print("  [LLM] HTTP 404:", resp.text[:200])
        return None
    if resp.status_code != 200:
        print("  [LLM] HTTP", resp.status_code, ":", resp.text[:150])
        return None

    # Responses API SSE stream
    full = ""
    for raw_line in resp.iter_lines(decode_unicode=False):
        if not raw_line:
            continue
        line = raw_line.decode("utf-8", errors="replace").strip()
        if not line.startswith("data: "):
            continue
        json_str = line[6:].strip()
        if json_str == "[DONE]":
            break
        try:
            ev = json.loads(json_str)
        except json.JSONDecodeError:
            continue
        ev_type = ev.get("type", "")
        if ev_type == "response.output_text.delta":
            delta = ev.get("delta", "")
            if delta:
                full += delta
                print(delta, end="", flush=True)
        elif ev_type == "response.done":
            break

    return full if full else None


# ============================================================
# 主流程
# ============================================================

def main():
    print("=" * 62)
    print("  Volcano Engine Smart Q&A - Full Pipeline Demo")
    print("  Flow: TTS -> ASR -> LLM -> TTS")
    print("=" * 62)

    config = load_config()

    questions = [
        "今天深圳天气怎么样",
        "你是谁啊",
        "你是一个资深老师，简单的说一下学习的重要性？",
    ]

    out_dir = "output"
    os.makedirs(out_dir, exist_ok=True)
    transcript_lines = []
    total_start = time.time()

    for idx, question in enumerate(questions, 1):
        print()
        print("-" * 62)
        print("Q" + str(idx) + ": " + question)
        print("-" * 62)

        # ---- Step 1: TTS question ----
        q_wav = os.path.join(out_dir, "%02d_question.wav" % idx)
        print("\n  [1/4] TTS: text -> speech ...", end=" ", flush=True)
        t0 = time.time()

        if tts_speak(config, question, q_wav):
            print("OK (%.1fs) -> %s" % (time.time() - t0, q_wav))
        else:
            print("FAIL")
            continue

        # ---- Step 2: ASR recognition ----
        print("  [2/4] ASR: speech -> text ...", end=" ", flush=True)
        t0 = time.time()

        asr_text = asr_recognize(config, q_wav)
        if asr_text:
            print("OK (%.1fs)" % (time.time() - t0))
            print("         Result: " + asr_text)
        else:
            print("FAIL")
            continue

        # ---- Step 3: LLM answer ----
        print("  [3/4] LLM: question -> answer ...", end=" ", flush=True)
        t0 = time.time()

        ans_text = llm_chat(config, asr_text)
        if ans_text:
            elapsed = time.time() - t0
            print()
            print("                    (%.1fs)" % elapsed)
            print("         Answer: " + ans_text[:80] + ("..." if len(ans_text) > 80 else ""))
        else:
            print("FAIL")
            continue

        # ---- Step 4: TTS answer ----
        a_wav = os.path.join(out_dir, "%02d_answer.wav" % idx)
        print("  [4/4] TTS: answer -> speech ...", end=" ", flush=True)
        t0 = time.time()

        if tts_speak(config, ans_text, a_wav):
            print("OK (%.1fs) -> %s" % (time.time() - t0, a_wav))
        else:
            print("FAIL")

        transcript_lines.append("Q%d: %s" % (idx, question))
        transcript_lines.append("ASR: " + asr_text)
        transcript_lines.append("LLM: " + ans_text)
        transcript_lines.append("WAV: " + q_wav + " | " + a_wav)
        transcript_lines.append("")

    if transcript_lines:
        transcript_path = os.path.join(out_dir, "transcript.txt")
        with open(transcript_path, "w", encoding="utf-8") as f:
            f.write("\n".join(transcript_lines))
        total = time.time() - total_start
        print()
        print("=" * 62)
        print("DONE! Total: %.1fs" % total)
        print("    Output: " + os.path.abspath(out_dir))
        print("    Transcript: " + transcript_path)
    else:
        print()
        print("=" * 62)
        print("[ERR] All questions failed, please check config")


if __name__ == "__main__":
    main()
