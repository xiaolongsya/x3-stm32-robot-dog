# schema.py — STM32 命令枚举 + JSON Schema (LLM 输出约束)
"""
2026-09-16 立项:
- 蜂鸣器命令 0x07/0x08 已废,本文件不包含
- MOTION_PLAY id=1..5 (5=WALK,需 direction)
- ACTION_PLAY id=5..8 (渐进 SIT/STAND)
- EMERGENCY_STOP id=0
"""
import re

# === STM32 命令码 (commands.c 一致) ===
CMD_MOTION_PLAY     = 0x01
CMD_SET_PWM         = 0x03
CMD_HEARTBEAT       = 0x05
CMD_EMERGENCY_STOP  = 0x06
CMD_ACTION_PLAY     = 0x09

# === LLM cmd 名 → STM32 命令码 ===
CMD_NAME_TO_STM32 = {
    "ACTION_PLAY":    CMD_ACTION_PLAY,
    "MOTION_PLAY":    CMD_MOTION_PLAY,
    "EMERGENCY_STOP": CMD_EMERGENCY_STOP,
}

# === Ollama JSON Schema (response_format.json_schema.schema) ===
ACTION_SCHEMA = {
    "type": "object",
    "properties": {
        "actions": {
            "type": "array",
            "maxItems": 16,
            "items": {
                "type": "object",
                "properties": {
                    "cmd": {
                        "type": "string",
                        "enum": ["ACTION_PLAY", "MOTION_PLAY", "EMERGENCY_STOP"],
                    },
                    "id": {"type": "integer", "minimum": 1, "maximum": 9},
                    "duration_ms": {"type": "integer", "minimum": 0, "maximum": 60000},
                    "direction": {"type": "integer", "minimum": -1, "maximum": 1},
                },
                "required": ["cmd"],
            },
        },
        "reply": {"type": "string", "maxLength": 100},
    },
    "required": ["actions", "reply"],
}


def validate_actions(raw_actions):
    """过滤非法动作,返回 (valid_list, error_list)。
    合法 cmd/id 通过,非法跳过。
    """
    valid = []
    errors = []
    for a in raw_actions:
        if not isinstance(a, dict):
            errors.append(f"非对象: {a}")
            continue
        cmd = a.get("cmd")
        if cmd not in CMD_NAME_TO_STM32:
            errors.append(f"未知 cmd: {cmd}")
            continue
        if cmd == "ACTION_PLAY":
            id_ = a.get("id")
            if id_ not in (5, 6, 7, 8):
                errors.append(f"ACTION_PLAY id={id_} 越界(需 5..8)")
                continue
            # duration_ms = ramp 完成后保持时长(0 = 无限保持,不回 STAND)
            # 2026-09-17 修:此前该字段被整条丢弃,LLM 发"蹲下3秒"等于"蹲下"
            hold = a.get("duration_ms", 0)
            if not isinstance(hold, int) or hold < 0 or hold > 60000:
                hold = 0  # 兜底:非法值当"无限保持"
            valid.append({"cmd": cmd, "id": int(id_), "duration_ms": int(hold)})
        elif cmd == "MOTION_PLAY":
            id_ = a.get("id")
            if id_ not in (1, 2, 3, 4, 5):
                errors.append(f"MOTION_PLAY id={id_} 越界(需 1..5)")
                continue
            default_duration = 8000 if id_ == 5 else 5000
            duration = a.get("duration_ms", default_duration)
            if not isinstance(duration, int) or duration < 0 or duration > 60000:
                duration = default_duration  # 兜底
            action = {"cmd": cmd, "id": int(id_), "duration_ms": int(duration)}
            if id_ == 5:
                direction = a.get("direction")
                if direction not in (-1, 1) or isinstance(direction, bool):
                    errors.append(f"WALK direction={direction} 非法(需 -1 或 1)")
                    continue
                action["direction"] = direction
            valid.append(action)
        elif cmd == "EMERGENCY_STOP":
            valid.append({"cmd": cmd})
    return valid, errors


_WALK_COMMAND = re.compile(
    r"^\s*(?:小龙[，,]?\s*)?(?:请|帮我)?\s*"
    r"(前进|向前走|往前走|后退|倒退|向后走|往后走)"
    r"\s*(?:(\d{1,2}|[一二三四五六七八九十两]{1,3})\s*秒(?:钟)?)?"
    r"\s*(?:一下|吧)?[。.!！\s]*$"
)
_CN_DIGITS = {"一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5,
              "六": 6, "七": 7, "八": 8, "九": 9}


def _walk_seconds(value):
    if value is None:
        return 8
    if value.isdigit():
        return int(value)
    if value == "十":
        return 10
    if "十" in value:
        left, right = value.split("十", 1)
        tens = _CN_DIGITS.get(left, 1) if left else 1
        ones = _CN_DIGITS.get(right, 0) if right else 0
        return tens * 10 + ones
    return _CN_DIGITS.get(value)


def parse_direct_walk(text):
    """明确的前进/后退命令直接转成单条动作,避免 LLM 改时长或追加动作。"""
    match = _WALK_COMMAND.fullmatch(text or "")
    if not match:
        return None
    seconds = _walk_seconds(match.group(2))
    if seconds is None or not 1 <= seconds <= 60:
        return None
    direction = 1 if match.group(1) in ("前进", "向前走", "往前走") else -1
    return {
        "actions": [{"cmd": "MOTION_PLAY", "id": 5,
                     "duration_ms": seconds * 1000, "direction": direction}],
        "reply": "好的",
    }
