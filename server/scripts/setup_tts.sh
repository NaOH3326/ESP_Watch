#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PYTHON="${PYTHON:-python3}"
if [ ! -d .venv ]; then
  "$PYTHON" -m venv .venv
fi
# shellcheck disable=SC1091
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r requirements.txt -r requirements-tts.txt

if [ ! -f .env ]; then
  cp .env.example .env
  echo "created .env from .env.example; edit tokens and DeepSeek key before production"
fi

VOICE="${PIPER_VOICE:-zh_CN-huayan-medium}"
MODEL_DIR="${PIPER_MODEL_DIR:-./data/models/piper}"
mkdir -p "$MODEL_DIR"
if ! find "$MODEL_DIR" -maxdepth 1 -name '*.onnx' -print -quit | grep -q .; then
  python -m piper.download_voices "$VOICE" --data-dir "$MODEL_DIR"
fi

echo
echo "Piper voice: $MODEL_DIR"
echo "Start: ./scripts/run.sh"
