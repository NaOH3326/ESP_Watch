"""FastAPI entry point for the ESP_Watch AI / media / file server."""

from __future__ import annotations

import asyncio
import json
import logging
import mimetypes
from pathlib import Path
from typing import Any
from urllib.parse import quote

from fastapi import Depends, FastAPI, File, Form, HTTPException, Request, UploadFile, WebSocket
from fastapi.responses import FileResponse, JSONResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field
from starlette.concurrency import iterate_in_threadpool
from starlette.websockets import WebSocketDisconnect

from .ai import AIError, AIService
from .asr import ASRError, ASRService
from .audio import AudioError
from .auth import admin_guard, check_watch_http, check_watch_ws
from .config import Settings
from .db import Database
from .logs import install_log_handler, LOG_BUFFER
from .media import MediaError, MediaService, iter_file, truncate_utf8
from .tts import TTSError, TTSService
from .ws_ai import handle_watch_ws
from .wsv2 import WSV2Error, stream_video

__version__ = "1.0.0"
logger = logging.getLogger("watch-server")

# The watch side has an MP3/WAV/AAC simple decoder; do not advertise FLAC/OGG
# files that it cannot play.
WATCH_AUDIO_FORMATS = {
    ".mp3": "mp3",
    ".wav": "wav",
    ".aac": "aac",
    ".m4a": "m4a",
}

STATIC_DIR = Path(__file__).parent / "static"


class ChatMessage(BaseModel):
    role: str
    content: str


class ChatRequest(BaseModel):
    messages: list[ChatMessage] = Field(default_factory=list)
    provider: str | None = None
    model: str | None = None
    temperature: float | None = None
    stream: bool = True


class TTSRequest(BaseModel):
    text: str
    voice: str | None = None


def _parse_range(value: str | None, size: int) -> tuple[int, int, bool]:
    if not value:
        return 0, max(0, size - 1), False
    if not value.startswith("bytes=") or "," in value:
        raise HTTPException(416, detail="invalid range")
    spec = value[6:].strip()
    if "-" not in spec:
        raise HTTPException(416, detail="invalid range")
    left, right = spec.split("-", 1)
    try:
        if left == "":
            suffix = int(right)
            if suffix <= 0:
                raise ValueError
            start = max(0, size - suffix)
            end = size - 1
        else:
            start = int(left)
            end = int(right) if right else size - 1
    except ValueError as exc:
        raise HTTPException(416, detail="invalid range") from exc
    if start < 0 or start >= size or end < start:
        raise HTTPException(416, detail="range not satisfiable")
    end = min(end, size - 1)
    return start, end, True


def _disposition(kind: str, filename: str) -> str:
    mode = "inline" if kind in {"video", "image", "audio"} else "attachment"
    return "%s; filename*=UTF-8''%s" % (mode, quote(filename or "file"))


def create_app(
    settings: Settings | None = None,
    *,
    ai_service: Any | None = None,
    tts_service: Any | None = None,
    asr_service: Any | None = None,
) -> FastAPI:
    settings = settings or Settings.from_env()
    settings.ensure_dirs()
    log_level = getattr(logging, settings.log_level, logging.INFO)
    logging.basicConfig(level=log_level)
    logging.getLogger("watch-server").setLevel(log_level)
    install_log_handler(log_level)

    db = Database(settings.db_path)
    ai = ai_service or AIService(settings)
    tts = tts_service or TTSService(settings)
    media = MediaService(settings, db)
    asr = asr_service or ASRService(settings)
    admin = admin_guard(settings)
    try:
        reconciled = media.reconcile()
        if reconciled:
            logging.getLogger("uvicorn.error").info(
                "media reconcile: imported %d orphan file(s)", reconciled
            )
    except Exception:  # noqa: BLE001 - startup must survive a damaged file
        logging.getLogger("uvicorn.error").exception("media reconcile failed")

    app = FastAPI(title="ESP_Watch Server", version=__version__)
    app.state.settings = settings
    app.state.db = db
    app.state.ai = ai
    app.state.tts = tts
    app.state.media = media
    app.state.asr = asr

    @app.middleware("http")
    async def security_headers(request: Request, call_next):
        response = await call_next(request)
        response.headers.setdefault("X-Content-Type-Options", "nosniff")
        response.headers.setdefault("Referrer-Policy", "no-referrer")
        response.headers.setdefault(
            "Content-Security-Policy",
            "default-src 'self'; img-src 'self' data: blob:; "
            "media-src 'self' blob:; style-src 'self' 'unsafe-inline'; "
            "script-src 'self'; connect-src 'self'",
        )
        if request.url.path.startswith(("/docs", "/redoc")):
            # Swagger UI/ReDoc use an inline bootstrap script and CDN assets.
            response.headers.pop("Content-Security-Policy", None)
        return response

    @app.get("/healthz")
    async def healthz():
        return {"status": "ok", "version": __version__}

    @app.get("/api/status")
    async def status():
        return {
            "status": "ok",
            "version": __version__,
            "config": settings.public_summary(),
            "ai": await ai.provider_status(),
            "tts": tts.status(),
            "asr": asr.status(),
            "media": db.stats(),
        }

    @app.get("/api/logs", dependencies=[Depends(admin)])
    async def logs(after: int = 0, limit: int = 200):
        items, cursor = LOG_BUFFER.snapshot(after=after, limit=limit)
        return {"items": items, "cursor": cursor}

    @app.get("/api/ai/providers", dependencies=[Depends(admin)])
    async def ai_providers():
        return await ai.provider_status()

    def sse(payload: dict[str, Any]) -> str:
        return "data: " + json.dumps(payload, ensure_ascii=False) + "\n\n"

    @app.post("/api/chat", dependencies=[Depends(admin)])
    async def chat(request: ChatRequest):
        messages = [message.model_dump() for message in request.messages]
        if not any(message.get("role") == "system" for message in messages):
            messages.insert(0, {"role": "system", "content": settings.system_prompt})
        if not request.stream:
            chunks: list[str] = []
            try:
                async for chunk in ai.stream(
                    messages,
                    provider=request.provider,
                    model=request.model,
                    temperature=request.temperature,
                ):
                    chunks.append(chunk)
            except AIError as exc:
                raise HTTPException(exc.status_code, detail=str(exc)) from exc
            return {"reply": "".join(chunks), "provider": ai.resolve_provider(request.provider)}

        async def event_stream():
            try:
                async for chunk in ai.stream(
                    messages,
                    provider=request.provider,
                    model=request.model,
                    temperature=request.temperature,
                ):
                    yield sse({"type": "delta", "text": chunk})
                yield sse({"type": "done"})
            except AIError as exc:
                yield sse({"type": "error", "message": str(exc)})
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001
                logger.exception("chat stream failed")
                yield sse({"type": "error", "message": "server error: %s" % str(exc)[:160]})

        return StreamingResponse(
            event_stream(),
            media_type="text/event-stream",
            headers={
                "Cache-Control": "no-cache",
                "X-Accel-Buffering": "no",
                "Connection": "keep-alive",
            },
        )


    @app.get("/api/tts/voices", dependencies=[Depends(admin)])
    async def tts_voices():
        return {
            "voices": tts.voices(),
            "provider": settings.tts_provider,
            "active": tts.status().get("voice", ""),
        }

    @app.post("/api/tts/voices", dependencies=[Depends(admin)])
    async def upload_tts_voice(
        file: UploadFile = File(...),
        config: UploadFile | None = File(None),
        name: str = Form(""),
        activate: bool = Form(True),
        overwrite: bool = Form(False),
    ):
        voice = await tts.upload_voice(
            file,
            config,
            name=name,
            activate=activate,
            overwrite=overwrite,
        )
        return JSONResponse(status_code=201, content={"voice": voice, "voices": tts.voices()})

    @app.post("/api/tts/voices/{voice_name}/select", dependencies=[Depends(admin)])
    async def select_tts_voice(voice_name: str):
        voice = tts.select_voice(voice_name)
        return {"voice": voice, "voices": tts.voices()}

    @app.delete("/api/tts/voices/{voice_name}", dependencies=[Depends(admin)])
    async def delete_tts_voice(voice_name: str):
        result = tts.delete_voice(voice_name)
        return {**result, "voices": tts.voices()}

    @app.get("/api/tts/status", dependencies=[Depends(admin)])
    async def tts_status():
        return tts.status()

    @app.post("/api/tts", dependencies=[Depends(admin)])
    async def synthesize(request: TTSRequest):
        text = request.text.strip()
        if not text:
            raise HTTPException(400, detail="text is empty")
        if len(text) > 4000:
            raise HTTPException(413, detail="text is too long (max 4000)")
        wav = await asyncio.to_thread(tts.synthesize, text, request.voice)
        return Response(
            content=wav,
            media_type="audio/wav",
            headers={
                "Cache-Control": "no-store",
                "Content-Disposition": 'inline; filename="tts.wav"',
            },
        )


    @app.get("/api/files", dependencies=[Depends(admin)])
    async def list_files(kind: str = "", limit: int = 200):
        selected = kind.strip().lower() if kind else None
        if selected not in {None, "video", "image", "audio", "file"}:
            raise HTTPException(400, detail="invalid kind")
        items = [media.public(item) for item in db.list(selected, limit=limit)]
        return {"items": items, "stats": db.stats()}

    @app.post("/api/files", dependencies=[Depends(admin)])
    async def upload_file(
        file: UploadFile = File(...),
        kind: str = Form(""),
        title: str = Form(""),
    ):
        record = await media.save_upload(file, kind=kind, title=title)
        return JSONResponse(status_code=201, content=media.public(record))

    @app.delete("/api/files/{media_id}", dependencies=[Depends(admin)])
    async def delete_file(media_id: str):
        record = media.delete(media_id)
        return {"deleted": record["id"]}

    @app.api_route("/api/files/{media_id}/content", methods=["GET", "HEAD"], dependencies=[Depends(admin)])
    async def file_content(media_id: str, request: Request):
        record = media.get_or_404(media_id)
        path = media.path_for(record)
        if not path.is_file():
            raise HTTPException(404, detail="file is missing")
        size = path.stat().st_size
        start, end, partial = _parse_range(request.headers.get("range"), size)
        length = max(0, end - start + 1)
        headers = {
            "Content-Type": record["mime"] or "application/octet-stream",
            "Content-Length": str(length),
            "Accept-Ranges": "bytes",
            "Content-Disposition": _disposition(record["kind"], record["original_name"]),
            "Cache-Control": "private, max-age=3600",
        }
        if partial:
            headers["Content-Range"] = "bytes %d-%d/%d" % (start, end, size)
        status_code = 206 if partial else 200
        if request.method == "HEAD":
            return Response(status_code=status_code, headers=headers)
        return StreamingResponse(
            iter_file(path, start, end),
            status_code=status_code,
            media_type=headers["Content-Type"],
            headers=headers,
        )


    @app.get("/watch/api/v1/videos")
    async def watch_catalog(request: Request):
        check_watch_http(settings, request)
        items = []
        for record in db.list("video", limit=16):
            items.append({
                "id": record["id"],
                "title": truncate_utf8(record["title"], 47),
                "duration_ms": int(record.get("duration_ms") or 0),
                "width": int(record.get("width") or settings.watch_video_width),
                "height": int(record.get("height") or settings.watch_video_height),
                "stream_url": "/watch/api/v1/videos/%s/stream" % record["id"],
                # WebSocket is the preferred transport: one connection carries
                # interleaved JPEG + PCM, no HTTP chunk framing, and the watch
                # can stop by closing the socket immediately.
                "ws_url": "/watch/api/v1/videos/%s/ws" % record["id"],
            })
        return {"version": 1, "items": items}

    @app.get("/watch/api/v1/music")
    async def watch_music_catalog(request: Request):
        check_watch_http(settings, request)
        items = []
        for record in db.list("audio", limit=64):
            suffix = Path(record.get("original_name") or "").suffix.lower()
            fmt = WATCH_AUDIO_FORMATS.get(suffix)
            if not fmt:
                continue
            items.append({
                "id": record["id"],
                "title": truncate_utf8(record["title"], 47),
                "duration_ms": int(record.get("duration_ms") or 0),
                "size": int(record.get("size") or 0),
                "format": fmt,
                "stream_url": "/watch/api/v1/music/%s/stream" % record["id"],
            })
            if len(items) >= 24:
                break
        return {"version": 1, "items": items}

    @app.api_route("/watch/api/v1/music/{media_id}/stream", methods=["GET", "HEAD"])
    async def watch_music_stream(media_id: str, request: Request):
        check_watch_http(settings, request)
        record = db.get(media_id)
        if not record or record["kind"] != "audio":
            raise HTTPException(404, detail="music not found")
        suffix = Path(record.get("original_name") or "").suffix.lower()
        if suffix not in WATCH_AUDIO_FORMATS:
            raise HTTPException(404, detail="unsupported audio format")
        path = media.path_for(record)
        if not path.is_file():
            raise HTTPException(404, detail="music file is missing")

        size = path.stat().st_size
        start, end, partial = _parse_range(request.headers.get("range"), size)
        length = max(0, end - start + 1)
        content_type = (
            mimetypes.guess_type(record.get("original_name") or "")[0]
            or record.get("mime")
            or "application/octet-stream"
        )
        headers = {
            "Content-Type": content_type,
            "Content-Length": str(length),
            "Accept-Ranges": "bytes",
            "Cache-Control": "no-store",
            "Content-Disposition": "inline; filename*=UTF-8''%s" % quote(
                record.get("original_name") or "music"
            ),
        }
        if partial:
            headers["Content-Range"] = "bytes %d-%d/%d" % (start, end, size)
        if request.method == "HEAD":
            return Response(status_code=206 if partial else 200, headers=headers)
        return StreamingResponse(
            iter_file(path, start, end),
            status_code=206 if partial else 200,
            media_type=content_type,
            headers=headers,
        )

    @app.get("/watch/api/v1/videos/{media_id}/stream")
    async def watch_stream(media_id: str, request: Request):
        check_watch_http(settings, request)
        record = db.get(media_id)
        if not record or record["kind"] != "video":
            raise HTTPException(404, detail="video not found")
        path = media.path_for(record)
        if not path.is_file():
            raise HTTPException(404, detail="video file is missing")

        def frames():
            try:
                yield from stream_video(
                    path,
                    width=settings.watch_video_width,
                    height=settings.watch_video_height,
                    quality=settings.watch_video_quality,
                    audio_ms=settings.watch_audio_ms,
                    fps=settings.watch_video_fps,
                    enable_audio=settings.watch_video_audio,
                )
            except (GeneratorExit, ConnectionError):
                return
            except Exception:
                logger.exception("WSV2 stream failed for %s", media_id)

        return StreamingResponse(
            frames(),
            media_type="application/vnd.wsv2",
            headers={"Cache-Control": "no-store", "Connection": "close"},
        )

    @app.websocket("/watch/api/v1/videos/{media_id}/ws")
    async def watch_stream_ws(websocket: WebSocket, media_id: str):
        """Stream the same WSV2 frames as binary WebSocket messages.

        Each message contains exactly one 20-byte WSV2 header + payload.  The
        server waits for ``send_bytes`` backpressure, so the watch's TCP/WS
        receive window (and its audio queue) paces the transcode loop.  This
        avoids the old HTTP-chunked path where a slow watch could leave a
        producer thread blocked in the ASGI threadpool until its timeout.
        """
        if not await check_watch_ws(websocket, settings):
            logger.warning("watch video websocket rejected (bad token)")
            return
        await websocket.accept()
        record = db.get(media_id)
        if not record or record["kind"] != "video":
            await websocket.close(code=4404, reason="video not found")
            return
        path = media.path_for(record)
        if not path.is_file():
            await websocket.close(code=4404, reason="video file is missing")
            return

        frames = stream_video(
            path,
            width=settings.watch_video_width,
            height=settings.watch_video_height,
            quality=settings.watch_video_quality,
            audio_ms=settings.watch_audio_ms,
            fps=settings.watch_video_fps,
            enable_audio=settings.watch_video_audio,
        )
        logger.info("watch video ws start: client=%s title=%s",
                    websocket.client.host if websocket.client else "?",
                    record.get("title") or media_id)
        try:
            async for frame in iterate_in_threadpool(frames):
                await websocket.send_bytes(frame)
            await websocket.send_text('{"type":"eof"}')
            await websocket.close(code=1000)
        except (WebSocketDisconnect, RuntimeError):
            pass
        except Exception:
            logger.exception("WSV2 websocket stream failed for %s", media_id)
            try:
                await websocket.close(code=1011)
            except Exception:
                pass
        finally:
            close = getattr(frames, "close", None)
            if close:
                close()
            logger.info("watch video ws stop: %s", media_id)


    @app.websocket("/v1/ws")
    async def watch_ws(websocket: WebSocket):
        await handle_watch_ws(websocket, settings, ai, tts, asr)

    @app.websocket("/ai")
    async def watch_ws_alias(websocket: WebSocket):
        await handle_watch_ws(websocket, settings, ai, tts, asr)

    if STATIC_DIR.is_dir():
        app.mount("/assets", StaticFiles(directory=str(STATIC_DIR)), name="assets")

        @app.get("/")
        async def index():
            return FileResponse(STATIC_DIR / "index.html")

        @app.get("/dashboard")
        async def dashboard():
            return FileResponse(STATIC_DIR / "index.html")

        @app.get("/favicon.ico")
        async def favicon():
            icon = STATIC_DIR / "favicon.svg"
            if icon.is_file():
                return FileResponse(icon, media_type="image/svg+xml")
            return Response(status_code=204)

    @app.exception_handler(AIError)
    async def ai_error_handler(request: Request, exc: AIError):
        return JSONResponse(status_code=exc.status_code, content={"error": str(exc)})

    @app.exception_handler(TTSError)
    async def tts_error_handler(request: Request, exc: TTSError):
        return JSONResponse(status_code=exc.status_code, content={"error": str(exc)})

    @app.exception_handler(ASRError)
    async def asr_error_handler(request: Request, exc: ASRError):
        return JSONResponse(status_code=exc.status_code, content={"error": str(exc)})

    @app.exception_handler(MediaError)
    async def media_error_handler(request: Request, exc: MediaError):
        return JSONResponse(status_code=exc.status_code, content={"error": str(exc)})

    @app.exception_handler(WSV2Error)
    async def wsv2_error_handler(request: Request, exc: WSV2Error):
        return JSONResponse(status_code=503, content={"error": str(exc)})

    @app.exception_handler(AudioError)
    async def audio_error_handler(request: Request, exc: AudioError):
        return JSONResponse(status_code=503, content={"error": str(exc)})

    return app


app = create_app()
