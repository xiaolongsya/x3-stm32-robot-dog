# llm.py — LLM 客户端 (Ollama 原生 API, xiaolong/qwen3:8b-q8_0)
"""2026-09-16:从 OpenAI 兼容端点切到 Ollama 原生 /api/chat,
因为只有原生 API 支持 think:false 关闭 Qwen3 thinking(否则延迟 6s vs 0.3s)。
"""
import asyncio
import json
import time

import httpx

from . import logger
from .schema import ACTION_SCHEMA, validate_actions

log = logger.get_logger("llm")

# Ollama 原生 API
OLLAMA_API = "http://127.0.0.1:11434/api/chat"


class LLMClient:
    def __init__(
        self,
        base_url: str = "http://127.0.0.1:11434",
        model: str = "xiaolong",
    ):
        self.base_url = base_url.rstrip("/")
        self.api_url = f"{self.base_url}/api/chat"
        self.model = model
        self._client: httpx.AsyncClient | None = None
        log.info(f"LLMClient init: model={model} api={self.api_url}")

    async def _ensure_client(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(timeout=10.0)
        return self._client

    async def close(self):
        if self._client:
            await self._client.aclose()
            self._client = None

    async def chat(self, system: str, user: str, timeout: float = 5.0):
        """返回 dict {"actions":[...], "reply": "..."}
        错误情况返回 actions=[], reply 为兜底文本
        """
        t0 = time.time()
        client = await self._ensure_client()

        # 用 1) think:false 关思考 (Ollama 原生 API 支持)
        content = None
        for attempt in range(2):
            try:
                payload = {
                    "model": self.model,
                    "messages": [
                        {"role": "system", "content": system},
                        {"role": "user", "content": user},
                    ],
                    "stream": False,
                    "think": False,            # 关 Qwen3 thinking (原生 API)
                    "format": ACTION_SCHEMA,    # 强制 schema 输出(原生 API 用 format 字段)
                    "options": {
                        "temperature": 0.1,
                        "num_predict": 300,
                    },
                }
                resp = await asyncio.wait_for(
                    client.post(self.api_url, json=payload),
                    timeout=timeout,
                )
                resp.raise_for_status()
                data = resp.json()
                content = (data.get("message") or {}).get("content") or ""
                break
            except asyncio.TimeoutError:
                log.warn(f"LLM timeout ({timeout}s) attempt={attempt}")
                return {"actions": [], "reply": "我反应慢了下次再说"}
            except Exception as e:
                log.warn(f"LLM error attempt={attempt}: {e}")
                if attempt == 1:
                    return {"actions": [], "reply": "我没听懂,再说一遍"}

        # 解析 JSON
        try:
            parsed = json.loads(content)
        except json.JSONDecodeError:
            log.error(f"LLM non-JSON: {content[:120]}")
            return {"actions": [], "reply": "我没听懂,再说一遍"}

        if not isinstance(parsed, dict):
            log.error(f"LLM returned non-dict: {type(parsed)}")
            return {"actions": [], "reply": "我没听懂"}

        # 验证 + 过滤
        raw_actions = parsed.get("actions", [])
        valid, errors = validate_actions(raw_actions)
        reply = str(parsed.get("reply", "")).strip()
        if errors:
            log.warn(f"LLM action errors: {errors}")
        truncated = False
        if len(valid) > 16:
            valid = valid[:16]
            truncated = True

        if not reply:
            reply = "好的"
        if truncated:
            reply = reply + " (后面的没听清)"

        t1 = time.time()
        log.info(
            f"LLM chat {int((t1 - t0) * 1000)}ms: actions={len(valid)} "
            f"reply='{reply[:40]}'"
        )
        return {"actions": valid, "reply": reply}