"""Small SQLite store for uploaded media metadata."""

from __future__ import annotations

import sqlite3
import threading
import time
from pathlib import Path
from typing import Any


class Database:
    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self._lock = threading.Lock()
        self.init()

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.path, timeout=10)
        conn.row_factory = sqlite3.Row
        return conn

    def init(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self._lock, self._connect() as conn:
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute(
                """
                CREATE TABLE IF NOT EXISTS media (
                    id TEXT PRIMARY KEY,
                    kind TEXT NOT NULL,
                    title TEXT NOT NULL,
                    filename TEXT NOT NULL,
                    original_name TEXT NOT NULL,
                    mime TEXT NOT NULL,
                    size INTEGER NOT NULL,
                    sha256 TEXT NOT NULL,
                    width INTEGER NOT NULL DEFAULT 0,
                    height INTEGER NOT NULL DEFAULT 0,
                    duration_ms INTEGER NOT NULL DEFAULT 0,
                    created_at REAL NOT NULL
                )
                """
            )
            conn.execute("CREATE INDEX IF NOT EXISTS idx_media_kind ON media(kind, created_at DESC)")

    @staticmethod
    def _row(row: sqlite3.Row | None) -> dict[str, Any] | None:
        return dict(row) if row is not None else None

    def add(
        self,
        *,
        media_id: str,
        kind: str,
        title: str,
        filename: str,
        original_name: str,
        mime: str,
        size: int,
        sha256: str,
        width: int = 0,
        height: int = 0,
        duration_ms: int = 0,
    ) -> dict[str, Any]:
        created = time.time()
        with self._lock, self._connect() as conn:
            conn.execute(
                """
                INSERT INTO media
                (id, kind, title, filename, original_name, mime, size, sha256,
                 width, height, duration_ms, created_at)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (media_id, kind, title, filename, original_name, mime, size,
                 sha256, width, height, duration_ms, created),
            )
            row = conn.execute("SELECT * FROM media WHERE id = ?", (media_id,)).fetchone()
        return dict(row)  # type: ignore[arg-type]

    def get(self, media_id: str) -> dict[str, Any] | None:
        with self._lock, self._connect() as conn:
            row = conn.execute("SELECT * FROM media WHERE id = ?", (media_id,)).fetchone()
        return self._row(row)

    def list(self, kind: str | None = None, limit: int = 200) -> list[dict[str, Any]]:
        limit = max(1, min(int(limit), 1000))
        with self._lock, self._connect() as conn:
            if kind:
                rows = conn.execute(
                    "SELECT * FROM media WHERE kind = ? ORDER BY created_at DESC LIMIT ?",
                    (kind, limit),
                ).fetchall()
            else:
                rows = conn.execute(
                    "SELECT * FROM media ORDER BY created_at DESC LIMIT ?", (limit,)
                ).fetchall()
        return [dict(row) for row in rows]

    def delete(self, media_id: str) -> bool:
        with self._lock, self._connect() as conn:
            cur = conn.execute("DELETE FROM media WHERE id = ?", (media_id,))
        return cur.rowcount > 0

    def stats(self) -> dict[str, int]:
        with self._lock, self._connect() as conn:
            rows = conn.execute(
                "SELECT kind, COUNT(*) AS n FROM media GROUP BY kind"
            ).fetchall()
            total = conn.execute("SELECT COUNT(*) FROM media").fetchone()[0]
        counts = {row["kind"]: int(row["n"]) for row in rows}
        counts["total"] = int(total)
        return counts
