#!/usr/bin/env python3
# brain.py — PC 端 brain 服务 (WS server + ASR + LLM 管线)
"""
2026-09-16 初版

启动:
  python brain.py                  # WS 模式(默认端口 8765)
  python brain.py --test tests/sample.wav  # 单测 wav → ASR → LLM → 打印动作

X3 ↔ PC WS 协议(JSON):
  X3 → PC: {"type":"utterance", "pcm": "<base64 16k int16 LE>"}
  X3 → PC: {"type":"ping", "ts": <ms>}
  PC → X3: {"type":"action", "actions":[...], "reply":"..."}
  PC → X3: {"type":"pong", "ts": <ms>}
  PC → X3: {"type":"error", "code":"ASR_FAIL|BAD_AUDIO|...", "reply":"..."}

会话:每个 X3 客户端一份 3 轮历史窗口
"""
import argparse
import asyncio
import base64
import json
import time

from . import logger
from .asr import ASRClient
from .llm import LLMClient
from .prompts import SYSTEM_PROMPT, build_user_prompt

log = logger.get_logger("brain")

# === WS 协议 type ===
MSG_UTTERANCE = "utterance"
MSG_ACTION    = "action"
MSG_PING      = "ping"
MSG_PONG      = "pong"
MSG_ERROR     = "error"

# === 调优 ===
LLM_TIMEOUT_S = 5.0
HISTORY_MAX   = 3


# === 会话(每个 X3 一份)===
class Session:
    def __init__(self):
        self.history = []  # 最近 N 轮 [{"user","actions","reply"}]

    def add_turn(self, user_text, actions, reply):
        self.history.append({
            "user": user_text,
            "actions": actions,
            "reply": reply,
        })
        if len(self.history) > HISTORY_MAX:
            self.history = self.history[-HISTORY_MAX:]


class BrainServer:
    def __init__(self, asr: ASRClient, llm: LLMClient):
        self.asr = asr
        self.llm = llm
        self.sessions = {}  # ws_id -> Session

    async def handle_client(self, ws):
        """每个 X3 客户端一个 coroutine"""
        ws_id = id(ws)
        sess = Session()
        self.sessions[ws_id] = sess
        log.info(f"X3 connected: ws_id={ws_id}")
        try:
            async for raw in ws:
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    log.warn(f"ws_id={ws_id} BAD_JSON: {raw[:80]}")
                    await ws.send(json.dumps({
                        "type": MSG_ERROR,
                        "code": "BAD_JSON",
                        "reply": "格式错误",
                    }))
                    continue
                mtype = msg.get("type")
                if mtype == MSG_UTTERANCE:
                    pcm_b64 = msg.get("pcm", "")
                    try:
                        pcm_bytes = base64.b64decode(pcm_b64)
                    except Exception as e:
                        log.warn(f"ws_id={ws_id} BAD_AUDIO: {e}")
                        await ws.send(json.dumps({
                            "type": MSG_ERROR,
                            "code": "BAD_AUDIO",
                            "reply": "音频数据错误",
                        }))
                        continue
                    await self._process_utterance(ws, ws_id, sess, pcm_bytes)
                elif mtype == MSG_PING:
                    await ws.send(json.dumps({
                        "type": MSG_PONG,
                        "ts": msg.get("ts"),
                    }))
                else:
                    log.warn(f"ws_id={ws_id} unknown type: {mtype}")
        except Exception as e:
            log.error(f"ws_id={ws_id} error: {e}")
        finally:
            log.info(f"X3 disconnected: ws_id={ws_id}")
            self.sessions.pop(ws_id, None)

    async def _process_utterance(self, ws, ws_id, sess: Session, pcm_bytes):
        t0 = time.time()
        # 1) ASR
        t_asr0 = time.time()
        text = await asyncio.to_thread(self.asr.transcribe, pcm_bytes)
        asr_ms = int((time.time() - t_asr0) * 1000)
        if not text:
            await ws.send(json.dumps({
                "type": MSG_ERROR,
                "code": "ASR_FAIL",
                "reply": "我没听清,再说一遍",
            }))
            log.warn(f"[ws={ws_id}] ASR 空输出(可能是静音/噪声)")
            return
        log.info(f"[ws={ws_id}] ASR {asr_ms}ms: '{text}'")
        # 2) LLM
        t_llm0 = time.time()
        user_prompt = build_user_prompt(sess.history, text)
        result = await self.llm.chat(SYSTEM_PROMPT, user_prompt, timeout=LLM_TIMEOUT_S)
        llm_ms = int((time.time() - t_llm0) * 1000)
        actions = result["actions"]
        reply = result["reply"]
        # LLM 解析详情
        actions_short = ", ".join(
            f"{a['cmd']}#{a.get('id','')}" + (f"/{a.get('duration_ms')}ms" if 'duration_ms' in a else "")
            for a in actions
        ) or "(空)"
        log.info(
            f"[ws={ws_id}] LLM  {llm_ms}ms: actions=[{actions_short}] reply='{reply}'"
        )
        # 3) 历史
        sess.add_turn(text, actions, reply)
        # 4) 发给 X3
        await ws.send(json.dumps({
            "type": MSG_ACTION,
            "actions": actions,
            "reply": reply,
        }))
        total_ms = int((time.time() - t0) * 1000)
        log.info(
            f"[ws={ws_id}] total {total_ms}ms (asr={asr_ms} + llm={llm_ms})"
        )


async def run_ws_server(port: int = 8765):
    import websockets
    log.info("Initializing ASR (SenseVoiceSmall)...")
    asr = ASRClient()
    log.info("Initializing LLM (qwen3:8b)...")
    llm = LLMClient()
    brain = BrainServer(asr, llm)
    log.info(f"Starting WS server on 0.0.0.0:{port} ...")
    async with websockets.serve(brain.handle_client, "0.0.0.0", port):
        log.info(f"WS ready. Waiting for X3 clients at ws://0.0.0.0:{port}")
        await asyncio.Future()  # 永久运行


def run_test(wav_path: str):
    """单测:wav → ASR → LLM → 打印"""
    import soundfile as sf

    log.info(f"=== Test mode: {wav_path} ===")
    log.info("Initializing ASR (SenseVoiceSmall)...")
    asr = ASRClient()
    log.info("Initializing LLM (qwen3:8b)...")
    llm = LLMClient()

    pcm, sr = sf.read(wav_path, dtype="int16")
    if sr != 16000:
        log.warn(f"sample rate {sr} != 16000,可能影响 ASR 精度")
    pcm_bytes = pcm.tobytes()

    t0 = time.time()
    text = asr.transcribe(pcm_bytes)
    if not text:
        log.warn("ASR returned empty")
        return
    log.info(f"ASR: '{text}'")

    user_prompt = build_user_prompt([], text)
    result = asyncio.run(llm.chat(SYSTEM_PROMPT, user_prompt))
    t1 = time.time()

    print("=" * 60)
    print(f"用户: {text}")
    print(f"回复: {result['reply']}")
    print(f"动作 ({len(result['actions'])} 个):")
    for a in result["actions"]:
        if "duration_ms" in a:
            print(f"  - {a['cmd']} id={a['id']} duration_ms={a['duration_ms']}")
        else:
            print(f"  - {a['cmd']} id={a.get('id', '')}")
    print("=" * 60)
    print(f"总耗时: {int((t1 - t0) * 1000)}ms")


def main():
    ap = argparse.ArgumentParser(description="PC 端 brain 服务")
    ap.add_argument("--port", type=int, default=8765, help="WS 端口(默认 8765)")
    ap.add_argument("--test", type=str, help="单测 wav 文件(不起 WS)")
    args = ap.parse_args()
    if args.test:
        run_test(args.test)
    else:
        try:
            asyncio.run(run_ws_server(args.port))
        except KeyboardInterrupt:
            log.info("Bye")


if __name__ == "__main__":
    main()