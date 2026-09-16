# asr.py — ASR 客户端 (FunASR SenseVoiceSmall)
"""2026-09-16:初版 FP16 半精度 + ASR 后处理(数字归一 + 同义词纠正)"""
import re
import time

import numpy as np

from . import logger

log = logger.get_logger("asr")

# === ASR 后处理 ===
# SenseVoiceSmall 中文 ASR 会把数字 "3" 听成 "三","5" 听成 "五" 等。还会字面误识:
# "立正" → "李正", "起立" → "其立"。这里做归一 + 纠正,让 LLM parse 更稳。

_CN_NUM = {
    "零": 0, "〇": 0,
    "一": 1, "壹": 1,
    "二": 2, "两": 2, "贰": 2,
    "三": 3, "叁": 3,
    "四": 4, "肆": 4,
    "五": 5, "伍": 5,
    "六": 6, "陆": 6,
    "七": 7, "柒": 7,
    "八": 8, "捌": 8,
    "九": 9, "玖": 9,
    "十": 10,
}


def _cn_digit_to_int(s: str) -> int | None:
    """中文数字转 int (支持"十三"=13)
    简单实现:支持 0-99。'十一'~'十九' = 11-19;'二十'~'九十九' = 20-99;
    '十' = 10;'一'..'九' = 1-9;'零/〇' = 0。"""
    if not s:
        return None
    if s == "十":
        return 10
    if len(s) == 1 and s in _CN_NUM:
        return _CN_NUM[s]
    # 十几
    if len(s) == 2 and s[0] == "十" and s[1] in _CN_NUM:
        return 10 + _CN_NUM[s[1]]
    # X十
    if len(s) >= 2 and s[1] == "十" and s[0] in _CN_NUM:
        base = _CN_NUM[s[0]] * 10
        if len(s) == 2:
            return base
        if len(s) == 3 and s[2] in _CN_NUM:
            return base + _CN_NUM[s[2]]
    return None


def normalize_cn_numbers(text: str) -> str:
    """中文数字串 → 阿拉伯数字串。"三秒" → "3秒","五秒钟" → "5秒钟","三次" → "3次"。"""
    if not text:
        return text

    def _replace(m):
        cn = m.group(0)
        n = _cn_digit_to_int(cn)
        return str(n) if n is not None else cn

    # 中文数字串(1-3 字符)。用非贪婪匹配避免跨词。
    # 跳过"一"和"两"(量词概率太高:一下/一个/一点/两个/两只)
    pattern = r"[零〇二三四五六七八九十]{1,3}"
    return re.sub(pattern, _replace, text)


# === 同义词纠正 ===
# 只针对常见 ASR 误识,不宽泛改写。
_COMMON_FIXES = [
    # 短词误识(立/李 起/其)
    (r"李正", "立正"),
    (r"其立", "起立"),
    (r"其下", "起下"),
    (r"尊下", "蹲下"),
    (r"尊吓", "蹲下"),
    # 句首"!"被吞
    (r"^(立正)(?![一-龥])", r"立正!"),  # 不强行加,留给 LLM
    # 丢问号
    # (不补,LLM 通常从语境能判断)
]


def correct_common_errors(text: str) -> str:
    """常见 ASR 误识纠正。"""
    if not text:
        return text
    for pat, rep in _COMMON_FIXES:
        if "!" in rep:
            continue  # 跳过带感叹号的(怕误加)
        text = re.sub(pat, rep, text)
    return text


def postprocess_asr_text(text: str) -> str:
    """ASR 输出后处理主入口:数字归一 + 同义词纠正"""
    if not text:
        return text
    text = normalize_cn_numbers(text)
    text = correct_common_errors(text)
    return text


class ASRClient:
    def __init__(self, device: str = "cuda:0"):
        from funasr import AutoModel

        log.info("Loading SenseVoiceSmall ...")
        t0 = time.time()
        # SenseVoiceSmall 支持 FP16/INT8;funasr 用 quantize=True (INT8)
        # FP32 ~1.5GB → INT8 ~400MB,质量几乎不损(< 1%)
        self.model = AutoModel(
            model="iic/SenseVoiceSmall",
            device=device,
            disable_update=True,
            quantize=True,
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
            # ASR 后处理:数字归一 + 同义词纠正
            text = postprocess_asr_text(text)
            t1 = time.time()
            log.info(f"ASR {int((t1 - t0) * 1000)}ms: '{text[:60]}'")
            return text if text else None
        except Exception as e:
            log.error(f"ASR error: {e}")
            return None