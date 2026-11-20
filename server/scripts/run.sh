#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -x .venv/bin/uvicorn ]; then
  exec .venv/bin/uvicorn app.main:app --host "${HOST:-0.0.0.0}" --port "${PORT:-8080}"
fi
exec uvicorn app.main:app --host "${HOST:-0.0.0.0}" --port "${PORT:-8080}"
