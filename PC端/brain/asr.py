# asr.py — ASR 客户端 (FunASR SenseVoiceSmall)
"""2026-09-16:初版, FP16 半精度"""
import re
import time

import numpy as np

from . import logger

log = logger.get_logger("asr")


class ASRClient:
    def __init__(self, device: str = "cuda:0"):
        from funasr import AutoModel

        log.info("Loading SenseVoiceSmall ...")
        t0 = time.time()
        # SenseVoiceSmall 支持 FP16 推理;FunASR 通过 quantize 参数控制
        # device=cuda:0 时,模型默认 FP32;手动 fp16 用 torch_dtype
        self.model = AutoModel(
            model="iic/SenseVoiceSmall",
            device=device,
            disable_update=True,
            # funasr 1.2+ 支持 torch_dtype / quantize;这里按默认 FP32 跑(显存够)
        )
        log.info(f"SenseVoiceSmall loaded {int((time.time() - t0) * 1000)}ms")

    def transcribe(self, pcm_bytes: bytes, sample_rate: int = 16000) -> str | None:
        """pcm_bytes: 16k int16 LE 单声道
        返回:转写文本(str),失败返回 None
        """
        if not pcm_bytes or len(pcm_bytes) < 1600:  # < 0.1s
            log.warn("ASR input too short")
            return None
        t0 = time.time()
        try:
            samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32) / 32768.0
            res = self.model.generate(
                input=samples,
                cache={},
            )
            text = res[0]["text"] if res else ""
            # 清理 SenseVoice 特殊 token (<|zh|><|HAPPY|><|NEUTRAL|> 等)
            text = re.sub(r"<\|[^|]+\|>", "", text).strip()
            t1 = time.time()
            log.info(f"ASR {int((t1 - t0) * 1000)}ms: '{text[:60]}'")
            return text if text else None
        except Exception as e:
            log.error(f"ASR error: {e}")
            return None