# tests/ — 单测音频

放 16kHz / 单声道 / 16-bit PCM wav 文件,命名建议:`sit.wav` / `stand.wav` / `trot_3s.wav` / `unknown.wav`。

## 怎么录

Windows 上可以用 `ffmpeg` + 默认麦克风,或 PowerShell 直接录:

```powershell
# ffmpeg 录 3 秒 16k 单声道 wav
ffmpeg -f dshow -i audio="Microphone (Realtek Audio)" -ar 16000 -ac 1 -t 3 sit.wav
```

## 测试用例建议

- `sit.wav`:说"请坐下"
- `stand.wav`:说"站起来"
- `combo.wav`:说"走两步再坐下"
- `unknown.wav`:说"翻个跟头"(测试兜底回复"我还不会")
- `reference.wav`:说"再站起来"(需要先有一条 stand.wav 历史,测试多轮指代)

## 跑

```bash
python brain.py --test tests/sit.wav
python test_client.py --wav tests/sit.wav
```