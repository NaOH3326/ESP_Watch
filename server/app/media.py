"""Uploaded media storage and light-weight metadata probing."""

from __future__ import annotations

import asyncio
import hashlib
import mimetypes
import os
import re
import secrets
from pathlib import Path
from typing import Any

from fastapi import UploadFile

from .config import Settings
from .db import Database
from .wsv2 import probe_video

VIDEO_EXTS = {".mp4", ".m4v", ".mov", ".webm", ".mkv", ".avi", ".ts"}
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".avif"}
AUDIO_EXTS = {".mp3", ".wav", ".ogg", ".oga", ".opus", ".m4a", ".aac", ".flac"}


class MediaError(RuntimeError):
    def __init__(self, message: str, status_code: int = 400) -> None:
        super().__init__(message)
        self.status_code = status_code


def classify(filename: str) -> str:
    suffix = Path(filename).suffix.lower()
    if suffix in VIDEO_EXTS:
        return "video"
    if suffix in IMAGE_EXTS:
        return "image"
    if suffix in AUDIO_EXTS:
        return "audio"
    return "file"


def truncate_utf8(text: str, limit: int) -> str:
    data = (text or "").encode("utf-8")
    if len(data) <= limit:
        return text
    return data[:limit].decode("utf-8", "ignore")


def iter_file(path: Path, start: int, end: int, chunk_size: int = 256 * 1024):
    remaining = max(0, end - start + 1)
    with path.open("rb") as handle:
        handle.seek(start)
        while remaining > 0:
            chunk = handle.read(min(chunk_size, remaining))
            if not chunk:
                break
            remaining -= len(chunk)
            yield chunk


class MediaService:
    def __init__(self, settings: Settings, db: Database) -> None:
        self.settings = settings
        self.db = db

    def _safe_name(self, filename: str) -> str:
        name = Path(filename or "upload.bin").name
        name = re.sub(r"[\x00-\x1f\x7f]", "_", name).strip() or "upload.bin"
        return name[:180]

    def _mime_for(self, original_name: str, kind: str) -> str:
        if kind == "file":
            return "application/octet-stream"
        guessed = mimetypes.guess_type(original_name)[0]
        if guessed:
            if kind == "image" and guessed == "image/svg+xml":
                return "application/octet-stream"
            return guessed
        return {
            "video": "video/mp4",
            "image": "image/jpeg",
            "audio": "audio/mpeg",
        }.get(kind, "application/octet-stream")

    def reconcile(self) -> int:
        """Import files that exist on disk but are missing from SQLite.

        This makes restarts safe after a lost/corrupt database and recovers
        uploads written before the metadata transaction completed.
        """
        known = {item["filename"] for item in self.db.list(limit=1000)}
        imported = 0
        try:
            paths = sorted(self.settings.files_dir.iterdir())
        except OSError:
            return 0
        for path in paths:
            if not path.is_file() or path.name.startswith("."):
                continue
            if path.name in known:
                continue
            try:
                size = path.stat().st_size
                if size <= 0:
                    continue
                digest = hashlib.sha256()
                with path.open("rb") as handle:
                    for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                        digest.update(chunk)
                kind = classify(path.name)
                media_id = path.stem if re.fullmatch(r"[0-9a-f]{12}", path.stem) else secrets.token_hex(6)
                while self.db.get(media_id):
                    media_id = secrets.token_hex(6)
                probe = self._probe(path, kind)
                self.db.add(
                    media_id=media_id,
                    kind=kind,
                    title=path.stem,
                    filename=path.name,
                    original_name=path.name,
                    mime=self._mime_for(path.name, kind),
                    size=size,
                    sha256=digest.hexdigest(),
                    width=probe.get("width", 0),
                    height=probe.get("height", 0),
                    duration_ms=probe.get("duration_ms", 0),
                )
                imported += 1
            except Exception:
                continue
        return imported

    async def save_upload(
        self,
        upload: UploadFile,
        *,
        kind: str = "",
        title: str = "",
    ) -> dict[str, Any]:
        original_name = self._safe_name(upload.filename or "upload.bin")
        suffix = Path(original_name).suffix.lower()
        suffix = re.sub(r"[^a-z0-9.]", "", suffix)[:10]
        chosen_kind = (kind or "").strip().lower()
        if chosen_kind not in {"video", "image", "audio", "file"}:
            chosen_kind = classify(original_name)
        if chosen_kind == "image" and suffix == ".svg":
            chosen_kind = "file"
        media_id = secrets.token_hex(6)
        while self.db.get(media_id):
            media_id = secrets.token_hex(6)
        stored_name = media_id + suffix
        final_path = self.settings.files_dir / stored_name
        temp_path = self.settings.files_dir / (".upload-" + media_id + ".part")
        digest = hashlib.sha256()
        size = 0
        max_bytes = self.settings.max_upload_bytes
        try:
            with temp_path.open("wb") as output:
                while True:
                    chunk = await upload.read(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > max_bytes:
                        raise MediaError(
                            "file is larger than %d MB" % self.settings.max_upload_mb,
                            413,
                        )
                    digest.update(chunk)
                    output.write(chunk)
            if size == 0:
                raise MediaError("empty upload", 400)
            os.replace(temp_path, final_path)
        except Exception:
            for path in (temp_path, final_path):
                try:
                    path.unlink()
                except OSError:
                    pass
            raise
        finally:
            await upload.close()

        probe = await asyncio.to_thread(self._probe, final_path, chosen_kind)
        display_title = (title or Path(original_name).stem or original_name).strip()[:120]
        try:
            record = self.db.add(
                media_id=media_id,
                kind=chosen_kind,
                title=display_title,
                filename=stored_name,
                original_name=original_name,
                mime=self._mime_for(original_name, chosen_kind),
                size=size,
                sha256=digest.hexdigest(),
                width=probe.get("width", 0),
                height=probe.get("height", 0),
                duration_ms=probe.get("duration_ms", 0),
            )
        except Exception:
            try:
                final_path.unlink()
            except OSError:
                pass
            raise
        return record


    def _probe(self, path: Path, kind: str) -> dict[str, int]:
        if kind == "video":
            return probe_video(path)
        if kind == "image":
            try:
                from PIL import Image  # type: ignore

                with Image.open(path) as image:
                    return {"width": int(image.width), "height": int(image.height), "duration_ms": 0}
            except Exception:
                return {"width": 0, "height": 0, "duration_ms": 0}
        return {"width": 0, "height": 0, "duration_ms": 0}

    def path_for(self, record: dict[str, Any]) -> Path:
        root = self.settings.files_dir.resolve()
        path = (root / record["filename"]).resolve()
        if path.parent != root:
            raise MediaError("invalid media path", 404)
        return path

    def get_or_404(self, media_id: str) -> dict[str, Any]:
        record = self.db.get(media_id)
        if not record:
            raise MediaError("media not found", 404)
        return record

    def delete(self, media_id: str) -> dict[str, Any]:
        record = self.get_or_404(media_id)
        try:
            self.path_for(record).unlink()
        except FileNotFoundError:
            pass
        except OSError as exc:
            raise MediaError("could not delete file: %s" % exc, 500) from exc
        self.db.delete(media_id)
        return record

    def public(self, record: dict[str, Any]) -> dict[str, Any]:
        item = dict(record)
        item.pop("filename", None)
        item["content_url"] = "/api/files/%s/content" % record["id"]
        return item
