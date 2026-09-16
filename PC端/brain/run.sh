#!/bin/bash
# run.sh — PC 端 brain 一键启动 (Git Bash / WSL / Linux / macOS)
# 2026-09-16

set -e

cd "$(dirname "$0")"

echo "[run] 1/4 检查 Ollama 服务..."
if ! curl -s http://127.0.0.1:11434/v1/models > /dev/null; then
    echo "[run] ✗ Ollama 没起来,先: ollama serve &  或装 Ollama Desktop"
    exit 1
fi

echo "[run] 2/4 检查 qwen3:8b 模型..."
if ! curl -s http://127.0.0.1:11434/v1/models | grep -q "qwen3:8b"; then
    echo "[run] 拉模型: ollama pull qwen3:8b"
    ollama pull qwen3:8b
fi

echo "[run] 3/4 激活 venv..."
if [ ! -d ".venv" ]; then
    echo "[run] 创建 venv ..."
    python -m venv .venv
    source .venv/bin/activate
    pip install --upgrade pip
    # PyTorch cu128 (Blackwell sm_120 要求)
    pip install torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cu128
    pip install -r requirements.txt
else
    source .venv/bin/activate
fi

echo "[run] 4/4 启动 brain 服务(默认端口 8765)..."
python brain.py "$@"