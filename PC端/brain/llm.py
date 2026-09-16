# llm.py — LLM 客户端 (Ollama OpenAI 兼容接口, qwen3:8b)
"""2026-09-16:初版"""
import asyncio
import json
import time

from openai import AsyncOpenAI

from . import logger
from .schema import ACTION_SCHEMA, validate_actions

log = logger.get_logger("llm")


class LLMClient:
    def __init__(
        self,
        base_url: str = "http://127.0.0.1:11434/v1",
        model: str = "qwen3:8b",
        api_key: str = "ollama",  # Ollama 兼容接口不校验
    ):
        self.client = AsyncOpenAI(base_url=base_url, api_key=api_key)
        self.model = model
        log.info(f"LLMClient init: model={model} base_url={base_url}")

    async def chat(self, system: str, user: str, timeout: float = 5.0):
        """返回 dict {"actions":[...], "reply": "..."}
        错误情况返回 actions=[], reply 为兜底文本
        """
        t0 = time.time()
        # 1) 调 LLM (优先用 json_schema,失败降级 json_object)
        content = None
        for attempt in range(2):
            try:
                if attempt == 0:
                    rf = {
                        "type": "json_schema",
                        "json_schema": {
                            "name": "dog_action",
                            "schema": ACTION_SCHEMA,
                            "strict": True,
                        },
                    }
                else:
                    rf = {"type": "json_object"}
                resp = await asyncio.wait_for(
                    self.client.chat.completions.create(
                        model=self.model,
                        messages=[
                            {"role": "system", "content": system},
                            {"role": "user", "content": user},
                        ],
                        response_format=rf,
                        temperature=0.3,
                        max_tokens=300,
                    ),
                    timeout=timeout,
                )
                content = resp.choices[0].message.content or ""
                break
            except asyncio.TimeoutError:
                log.warn(f"LLM timeout ({timeout}s) attempt={attempt}")
                return {"actions": [], "reply": "我反应慢了下次再说"}
            except Exception as e:
                log.warn(f"LLM error attempt={attempt}: {e}")
                if attempt == 1:
                    return {"actions": [], "reply": "我没听懂,再说一遍"}

        # 2) 解析 JSON (非 JSON 重试 1 次已在上面 attempt=1 完成)
        try:
            data = json.loads(content)
        except json.JSONDecodeError:
            log.error(f"LLM non-JSON after retry: {content[:120]}")
            return {"actions": [], "reply": "我没听懂,再说一遍"}

        if not isinstance(data, dict):
            log.error(f"LLM returned non-dict: {type(data)}")
            return {"actions": [], "reply": "我没听懂"}

        # 3) 验证 + 过滤
        raw_actions = data.get("actions", [])
        valid, errors = validate_actions(raw_actions)
        reply = str(data.get("reply", "")).strip()
        if errors:
            log.warn(f"LLM action errors: {errors}")
        # 截断超 8
        truncated = False
        if len(valid) > 8:
            valid = valid[:8]
            truncated = True

        # 4) 兜底 reply
        if not reply:
            reply = "好的"
        if truncated:
            reply = reply + " (后面的没听清)"

        t1 = time.time()
        log.info(f"LLM chat {int((t1 - t0) * 1000)}ms: actions={len(valid)} reply='{reply[:40]}'")
        return {"actions": valid, "reply": reply}