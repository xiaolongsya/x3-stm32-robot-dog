# rec_simple.ps1 — Windows 录音脚本(16kHz 单声道 16bit PCM wav)
# 2026-09-16:用户友好版,不依赖 ffmpeg
#
# 用法:
#   .\rec_simple.ps1 sit.wav 3       # 录 sit.wav 持续 3 秒
#   .\rec_simple.ps1 sit.wav         # 录到 Ctrl+C
#
# 优先用 ffmpeg(精确控制 16k/mono/16bit),ffmpeg 不在则用 .NET MediaRecorder(自动 resample)
# ffmpeg 装法: choco install ffmpeg

param(
    [Parameter(Mandatory=$true)][string]$Out,
    [int]$Sec = 0
)

$ScriptDir = =Path = Split-Path -Parent $MyInvocation.MyCommand.Path
$FullOut = if (Test-Path $Out -IsValid) { (Resolve-Path $Out).Path } else { Join-Path (Get-Location) $Out }

# === 优先 ffmpeg ===
if (Get-Command ffmpeg -ErrorAction SilentlyContinue) {
    $device = (Get-CimInstance Win32_SoundDevice | Where-Object { $_.Status -eq "OK" } | Select-Object -First 1).Caption
    Write-Host "[rec] 用 ffmpeg,麦克风: $device" -ForegroundColor Cyan
    if ($Sec -gt 0) {
        & ffmpeg -f dshow -i "audio=`"$device`" -ar 16000 -ac 1 -t $Sec $Out -y
    } else {
        & ffmpeg -f dshow -i "audio=`"$device`" -ar 16000 -ac 1 $Out -y
    }
    Write-Host "[rec] 保存: $Out" -ForegroundColor Green
    return
}

# === 备用: Windows MediaRecorder + post-resample ===
Write-Host "[rec] ffmpeg 不在,用 Windows MediaRecorder(自动 resample 到 16k)" -ForegroundColor Yellow
Write-Host "[rec] 强烈建议装 ffmpeg: choco install ffmpeg" -ForegroundColor Yellow

Add-Type -AssemblyName System.Windows.Forms

# 临时 44100 录音 + post-resample
$tmp = Join-Path $env:TEMP "rec_temp_$((Get-Date).Ticks).wav"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WavRec {
    [DllImport("winmm.dll", CharSet = CharSet.Auto)]
    public static extern int mciSendString(string command, System.Text.StringBuilder buffer, int bufferSize, IntPtr hwnd);
}
"@

$null = [WavRec]::mciSendString("open new type waveaudio alias myrec", $null, 0, [IntPtr]::Zero)
$null = [WavRec]::mciSendString("set myrec samplespersec 44100 channels 1 bitspersample 16", $null, 0, [IntPtr]::Zero)
$null = [WavRec]::mciSendString("record myrec", $null, 0, [IntPtr]::Zero)
Write-Host "[rec] 录音中..." -ForegroundColor Green
if ($Sec -gt 0) {
    Start-Sleep -Seconds $Sec
} else {
    Write-Host "按 Ctrl+C 停止"
    while ($true) { Start-Sleep -Seconds 1 }
}
$null = [WavRec]::mciSendString("stop myrec", $null, 0, [IntPtr]::Zero)
$null = [WavRec]::mciSendString("save myrec `"$tmp`"", $null, 0, [IntPtr]::Zero)
$null = [WavRec]::mciSendString("close myrec", $null, 0, [IntPtr]::Zero)

# 用 Python soundfile resample 到 16k
$py = "C:/Users/17402/Desktop/机器狗/PC端/brain/.venv/Scripts/python.exe"
& $py -c "
import soundfile as sf, numpy as np, sys
data, sr = sf.read(r'$tmp', dtype='int16')
if sr != 16000:
    # 简单线性 resample
    n_out = int(len(data) * 16000 / sr)
    data = np.interp(np.linspace(0, len(data), n_out), np.arange(len(data)), data.astype(float)).astype('int16')
sf.write(r'$FullOut', data, 16000, subtype='PCM_16')
print(f'保存: {r\"$FullOut\"} sr=16000 ch=1')
"
Remove-Item $tmp -ErrorAction SilentlyContinue