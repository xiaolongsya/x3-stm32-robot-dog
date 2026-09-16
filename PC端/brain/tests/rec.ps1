# rec.ps1 — Windows 上用 ffmpeg 录 16k 单声道 wav (给 PC brain 单测)
# 用法: .\rec.ps1 -Out sit.wav -Sec 3
#       .\rec.ps1 -Out sit.wav              # 录到 Ctrl+C
#
# 依赖: ffmpeg 在 PATH 里 (choco install ffmpeg 或下载 https://www.gyan.dev/ffmpeg/builds/)

param(
    [Parameter(Mandatory=$true)][string]$Out,
    [int]$Sec = 0
)

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Host "需要 ffmpeg。装法: choco install ffmpeg" -ForegroundColor Red
    exit 1
}

# Windows 上 ffmpeg 默认设备名 "Microphone"。多麦的话用 ffmpeg -list_devices true -f dshow -i dummy 查
$device = Get-CimInstance -ClassName Win32_SoundDevice |
    Where-Object { $_.Status -eq "OK" } |
    Select-Object -First 1 -ExpandProperty Caption
Write-Host "[rec] 使用麦克风: $device" -ForegroundColor Cyan
$devName = '"' + $device + ' (DirectSound)"'

if ($Sec -gt 0) {
    ffmpeg -f dshow -i "audio=$devName" -ar 16000 -ac 1 -t $Sec $Out -y
} else {
    ffmpeg -f dshow -i "audio=$devName" -ar 16000 -ac 1 $Out -y
}

Write-Host "[rec] 保存: $Out" -ForegroundColor Green