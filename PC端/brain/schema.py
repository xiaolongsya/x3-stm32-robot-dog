# schema.py — STM32 命令枚举 + JSON Schema (LLM 输出约束)
"""
2026-09-16 立项:
- 蜂鸣器命令 0x07/0x08 已废,本文件不包含
- MOTION_PLAY id=1..4 (motions 表)
- ACTION_PLAY id=5..8 (渐进 SIT/STAND)
- EMERGENCY_STOP id=0
"""

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
            if id_ not in (1, 2, 3, 4):
                errors.append(f"MOTION_PLAY id={id_} 越界(需 1..4)")
                continue
            duration = a.get("duration_ms", 5000)
            if not isinstance(duration, int) or duration < 0 or duration > 60000:
                duration = 5000  # 兜底
            valid.append({"cmd": cmd, "id": int(id_), "duration_ms": int(duration)})
        elif cmd == "EMERGENCY_STOP":
            valid.append({"cmd": cmd})
    return valid, errors