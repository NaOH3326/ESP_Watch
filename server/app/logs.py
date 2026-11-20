"""In-memory log ring buffer for the admin dashboard.

The dashboard polls /api/logs instead of opening a long-lived SSE connection.
That keeps the server stateless apart from this bounded deque and works through
Caddy/Cloudflare without special buffering headers.
"""

from __future__ import annotations

import logging
import threading
from collections import deque
from datetime import datetime, timezone
from typing import Any


class LogBuffer(logging.Handler):
    def __init__(self, capacity: int = 500) -> None:
        super().__init__()
        self.capacity = max(50, capacity)
        self._items: deque[dict[str, Any]] = deque(maxlen=self.capacity)
        self._lock = threading.Lock()
        self._next_id = 1

    def emit(self, record: logging.LogRecord) -> None:
        try:
            if record.name == "uvicorn.access" and "/api/logs" in record.getMessage():
                return
            message = record.getMessage()
            if record.exc_info:
                message += "\n" + self.formatException(record.exc_info)
            item = {
                "id": 0,
                "ts": datetime.fromtimestamp(record.created, timezone.utc)
                .astimezone()
                .isoformat(timespec="seconds"),
                "level": record.levelname,
                "logger": record.name,
                "message": message,
            }
            with self._lock:
                item["id"] = self._next_id
                self._next_id += 1
                self._items.append(item)
        except Exception:  # pragma: no cover - logging must never break requests
            self.handleError(record)

    def snapshot(self, after: int = 0, limit: int = 200) -> tuple[list[dict[str, Any]], int]:
        limit = max(1, min(int(limit or 200), self.capacity))
        with self._lock:
            items = list(self._items)
            cursor = self._next_id - 1
        if after > 0:
            items = [item for item in items if item["id"] > after]
        return items[-limit:], cursor


LOG_BUFFER = LogBuffer(500)
_HANDLER_INSTALLED = False


def install_log_handler(level: int = logging.INFO) -> LogBuffer:
    """Attach the buffer to the app and Uvicorn loggers exactly once."""
    global _HANDLER_INSTALLED
    LOG_BUFFER.setLevel(level)
    # Uvicorn configures its loggers with propagate=False, so the root handler
    # alone would miss WebSocket and access lines.  Attach to each target.
    for name in ("", "uvicorn.error", "uvicorn.access"):
        logger = logging.getLogger(name)
        logger.setLevel(level)
        if LOG_BUFFER not in logger.handlers:
            logger.addHandler(LOG_BUFFER)
    _HANDLER_INSTALLED = True
    return LOG_BUFFER
