# run.ps1 — PC 端 brain 一键启动 (PowerShell / Windows)
# 2026-09-16

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

function Step($n, $title) {
    Write-Host "[run] $n/4 $title" -ForegroundColor Cyan
}

Step 1 "检查 Ollama 服务 ..."
try {
    $r = Invoke-WebRequest -Uri "http://127.0.0.1:11434/v1/models" -UseBasicParsing -TimeoutSec 3
    if ($r.StatusCode -ne 200) { throw }
} catch {
    Write-Host "[run] ✗ Ollama 没起来,请先安装并启动: https://ollama.com/download" -ForegroundColor Red
    exit 1
}

Step 2 "检查 qwen3:8b 模型 ..."
$models = (Invoke-WebRequest -Uri "http://127.0.0.1:11434/v1/models" -UseBasicParsing).Content | ConvertFrom-Json
if ($models.data.id -notcontains "qwen3:8b") {
    Write-Host "[run] 拉模型 qwen3:8b ..." -ForegroundColor Yellow
    & ollama pull qwen3:8b
}

Step 3 "激活 venv ..."
if (-not (Test-Path ".venv")) {
    Write-Host "[run] 创建 venv ..." -ForegroundColor Yellow
    python -m venv .venv
    & .\.venv\Scripts\Activate.ps1
    python -m pip install --upgrade pip
    # PyTorch cu128 (Blackwell sm_120 要求)
    pip install torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu128
    pip install -r requirements.txt
} else {
    & .\.venv\Scripts\Activate.ps1
}

Step 4 "启动 brain 服务(默认端口 8765) ..."
python brain.py $args