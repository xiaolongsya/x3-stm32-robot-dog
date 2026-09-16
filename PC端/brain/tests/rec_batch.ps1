# rec_batch.ps1 — 录制 10 条语音命令测试集(按 Enter 录下一条)
# 2026-09-16
#
# 用法:
#   cd PC端/brain/tests
#   powershell -ExecutionPolicy Bypass -File .\rec_batch.ps1
#
# 流程:
#   1) 显示要说的文本
#   2) 按 Enter 开始录音(3 秒自动停)
#   3) 自动保存为 01.wav ~ 10.wav
#   4) 下一条
#
# 实测场景覆盖:
#   - 友好问句("你可以蹲下吗?")
#   - 复合命令("蹲下5秒再起来")
#   - 重复动作("蹲下起立5次")
#   - STM32 没能力的指令("摆动左腿"/"往前倾")→ LLM 应回"我还不会"
#   - 紧急停下("立正!")

param(
    [int]$Sec = 3
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir = Join-Path $ScriptDir "voice_commands"
$Py = Join-Path $ScriptDir "..\.venv\Scripts\python.exe"

# === 10 条测试命令 ===
$Commands = @(
    @{n="01"; text="你好呀,你可以蹲下吗?"},
    @{n="02"; text="踏步3秒"},
    @{n="03"; text="立正!"},
    @{n="04"; text="蹲下5秒钟再站起来把"},
    @{n="05"; text="摆动一下左前腿"},
    @{n="06"; text="摆动一下左肩部"},
    @{n="07"; text="蹲下起立5次"},
    @{n="08"; text="小狗狗,你可以蹲下吗?"},
    @{n="09"; text="小狗狗,蹲一个"},
    @{n="10"; text="往前倾一点"}
)

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " 10 条语音命令测试集录制" -ForegroundColor Cyan
Write-Host " 输出目录: $OutDir" -ForegroundColor Cyan
Write-Host " 每条录音时长: $Sec 秒(说完会自动停)" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "提示:" -ForegroundColor Yellow
Write-Host "  按 Enter 开始录音(给 0.5s 静默再说话,前奏模拟'唤醒后蹲下起立的延迟')" -ForegroundColor Gray
Write-Host "  ffmpeg 没装时自动用 Windows MediaRecorder fallback(降级到 resample)" -ForegroundColor Gray
Write-Host ""

# 检测录音工具
$hasFfmpeg = [bool](Get-Command ffmpeg -ErrorAction SilentlyContinue)
Write-Host "[检测] ffmpeg: $(if ($hasFfmpeg) {'✓ 已装'} else {'✗ 未装(用 MediaRecorder fallback)'})" -ForegroundColor $(if ($hasFfmpeg) {'Green'} else {'Yellow'})

# C# 代码(只在 ffmpeg 不可用时加载)
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public class WavRec {
    [DllImport("winmm.dll", CharSet = CharSet.Auto)]
    public static extern int mciSendString(string command, System.Text.StringBuilder buffer, int bufferSize, IntPtr hwnd);
}
"@ -ErrorAction SilentlyContinue

# === 10 条循环 ===
for ($i = 0; $i -lt $Commands.Count; $i++) {
    $cmd = $Commands[$i]
    $idx = $cmd.n
    $text = $cmd.text
    $outPath = Join-Path $OutDir "$idx.wav"

    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host "[$idx/10] 请说:" -ForegroundColor Cyan -NoNewline
    Write-Host " $text" -ForegroundColor Yellow
    Write-Host "按 Enter 开始录音 ($Sec 秒)..." -ForegroundColor Gray
    [void](Read-Host)

    if ($hasFfmpeg) {
        $device = (Get-CimInstance Win32_SoundDevice | Where-Object { $_.Status -eq "OK" } | Select-Object -First 1).Caption
        Write-Host "[录] ffmpeg $device ..."
        & ffmpeg -f dshow -i "audio=`"$device`"" -ar 16000 -ac 1 -t $Sec $outPath -y 2>&1 | Out-Null
    } else {
        $tmp = Join-Path $env:TEMP "rec_batch_$idx.wav"
        Write-Host "[录] MediaRecorder (44100 mono 16)..."
        $null = [WavRec]::mciSendString("open new type waveaudio alias myrec", $null, 0, [IntPtr]::Zero)
        $null = [WavRec]::mciSendString("set myrec samplespersec 44100 channels 1 bitspersample 16", $null, 0, [IntPtr]::Zero)
        $null = [WavRec]::mciSendString("record myrec", $null, 0, [IntPtr]::Zero)
        Start-Sleep -Seconds $Sec
        $null = [WavRec]::mciSendString("stop myrec", $null, 0, [IntPtr]::Zero)
        $null = [WavRec]::mciSendString("save myrec `"$tmp`"", $null, 0, [IntPtr]::Zero)
        $null = [WavRec]::mciSendString("close myrec", $null, 0, [IntPtr]::Zero)

        Write-Host "[录] resample 到 16k..."
        & $Py -c "
import soundfile as sf, numpy as np
data, sr = sf.read(r'$tmp', dtype='int16')
if sr != 16000:
    n = int(len(data) * 16000 / sr)
    data = np.interp(np.linspace(0, len(data), n), np.arange(len(data)), data.astype(float)).astype('int16')
sf.write(r'$outPath', data, 16000, subtype='PCM_16')
print(f'  -> sr=16000 ch=1 len={len(data)} samples ({len(data)/16000:.2f}s)')
"
        Remove-Item $tmp -ErrorAction SilentlyContinue
    }

    if (Test-Path $outPath) {
        $size = (Get-Item $outPath).Length
        Write-Host "[OK] $outPath ($size bytes)" -ForegroundColor Green
    } else {
        Write-Host "[ERR] 录制失败: $outPath" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " 全部 10 条录完" -ForegroundColor Green
Write-Host " 目录: $OutDir" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "下一步(你跑):" -ForegroundColor Yellow
Write-Host "   cd PC端" -ForegroundColor Gray
Write-Host "   brain\.venv\Scripts\python.exe -m brain.brain --test brain/tests/voice_commands/01.wav" -ForegroundColor Gray
Write-Host ""

Get-ChildItem $OutDir | Format-Table Name, Length -AutoSize