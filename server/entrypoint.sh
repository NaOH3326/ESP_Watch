#!/bin/sh
set -eu

MODEL_DIR="${PIPER_MODEL_DIR:-/data/models/piper}"
VOICE="${PIPER_VOICE:-zh_CN-huayan-medium}"

if [ "${AUTO_DOWNLOAD_PIPER:-1}" = "1" ] \
   && [ "${TTS_PROVIDER:-piper}" = "piper" ] \
   && [ -z "${PIPER_URL:-}" ]; then
  mkdir -p "$MODEL_DIR"
  if [ -n "${PIPER_MODEL:-}" ] && [ -f "$PIPER_MODEL" ]; then
    true
  elif ! find "$MODEL_DIR" -maxdepth 1 -name '*.onnx' -print -quit | grep -q .; then
    echo "[watch-server] Piper voice missing; downloading $VOICE ..."
    python -m piper.download_voices "$VOICE" --data-dir "$MODEL_DIR" \
      || echo "[watch-server] Piper download failed; TTS will report unavailable"
  fi
fi

exec "$@"
