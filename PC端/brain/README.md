# PC 端 brain 服务(2026-09-16)

桌面四足机器狗的 PC 端大脑:接收 X3 转发的语音 PCM,ASR 转写,LLM 解析为动作,回传 X3 转发 STM32 执行。

## 数据流

```
X3 (KWS 唤醒 + 录音)
   │  WS (ws://本机IP:8765)
   │  {"type":"utterance", "pcm":"<base64 16k int16 LE>"}
   ▼
[ brain.py ]
   ├─ ASR (FunASR SenseVoiceSmall) → text
   ├─ LLM (Ollama qwen3:8b, JSON Schema 约束)
   │   → {"actions":[...], "reply":"..."}
   ▼
X3 ← WS ← {"type":"action", "actions":[...], "reply":"..."}
   │
   ▼
STM32 (UART 二进制协议,cmd 0x01/0x06/0x09)
```

## 文件结构

```
PC端/brain/
├── brain.py          # 主入口(WS server + --test 单测)
├── llm.py            # LLM 客户端 (Ollama OpenAI 兼容)
├── asr.py            # ASR 客户端 (FunASR SenseVoiceSmall)
├── schema.py         # JSON Schema + 动作枚举 + validate
├── prompts.py         # LLM System prompt + User prompt 模板
├── logger.py         # 统一日志
├── test_client.py    # 模拟 X3 发 wav(WS 全链路测试)
├── requirements.txt  # 依赖
└── tests/            # 单测音频放这里
```

## 安装

### 1. 安装 Ollama(LLM 后端)

Windows 安装包: <https://ollama.com/download/OllamaSetup.exe>(系统服务,自启)

装完拉模型:

```bash
ollama pull qwen3:8b
```

验证服务:

```bash
curl http://127.0.0.1:11434/v1/models
```

### 2. Python 环境(本机,conda 或 venv 都行)

```bash
# venv
python -m venv .venv
.venv\Scripts\activate

# 装 PyTorch(cu128,Blackwell sm_120 要求)
pip install torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu128

# 装其他依赖
pip install -r requirements.txt
```

`funasr` 首次运行会自动下载 SenseVoiceSmall 模型(~230MB)到 `~/.cache/modelscope/hub/`。

### 3. 显存预估

- SenseVoiceSmall (FP32):~1.5 GB
- qwen3:8b (Q4_K_M via Ollama):~5.5 GB
- 合计 ~7 GB;RTX 5070 Ti 12GB 有 ~5GB 余量

## 启动

### WS 模式(等 X3 连)

```bash
python brain.py                 # 默认端口 8765
python brain.py --port 9000     # 自定义端口
```

输出示例:

```
17:30:01.123 [brain] INFO: Initializing ASR (SenseVoiceSmall)...
17:30:05.456 [asr]   INFO: SenseVoiceSmall loaded 4333ms
17:30:05.457 [brain] INFO: Initializing LLM (qwen3:8b)...
17:30:05.458 [llm]   INFO: LLMClient init: model=qwen3:8b
17:30:05.459 [brain] INFO: Starting WS server on 0.0.0.0:8765 ...
17:30:05.512 [brain] INFO: WS ready. Waiting for X3 clients at ws://0.0.0.0:8765
```

### --test 单测(不起 WS)

准备一段 16kHz 单声道 wav,跑:

```bash
python brain.py --test tests/sample.wav
```

输出:

```
============================================================
User: 坐下
回复: 好的,我坐下啦
动作 (1 个):
  - ACTION_PLAY id=5
============================================================
总耗时: 1832ms
```

### test_client.py 模拟 X3 全链路

另开一个终端(WS 服务先起着):

```bash
python test_client.py --wav tests/sample.wav
```

## 性能目标

- ASR 转写:< 500 ms
- LLM 推理:< 1.5 s
- 总延迟(说完到 X3 收到 action):< 2 s

每步耗时在日志里打印,便于对齐。

## 与 STM32 协议对应

LLM 输出的 `cmd` 字符串翻译为 STM32 UART 命令:

| LLM cmd | STM32 cmd | data |
|---|---|---|
| `MOTION_PLAY` | `0x01` | `[id u8, duration_ms u32 LE]` |
| `EMERGENCY_STOP` | `0x06` | `[]` |
| `ACTION_PLAY` | `0x09` | `[action_id u8, repeat u8]` |

翻译在 X3 端做(`dog_uart.py` 加 `brain_to_stm32` 函数)。`duration_ms > 0` 时 X3 端调 `MOTION_PLAY id=duration_ms/1000`。

## 已知约束

- Python 3.13.5(`funasr`/`torch` 已支持,但装 wheel 时盯一下)
- ASR 当前用 FP32,~1.5G 显存;FP16 可降到 ~0.7G 但需手动 fp16 cast
- LLM 关闭 thinking(Qwen3 默认开,prompt 头部已隐含)
- 单 X3 客户端(暂不支持多客户端 session 隔离)
- WS 服务端单进程(asyncio)