"""WatchShell AI WebSocket protocol (/v1/ws) for the ESP32 watch."""

from __future__ import annotations

import asyncio
import json
import logging
import time
import uuid
from typing import Any

from fastapi import WebSocket, WebSocketDisconnect

from .ai import AIError, AIService
from .asr import ASRError, ASRService
from .audio import AudioError, pcm16_to_opus_frames, wav_to_pcm16
from .auth import check_watch_ws
from .config import Settings
from .media import truncate_utf8
from .tts import TTSError, TTSService

logger = logging.getLogger("uvicorn.error")


async def handle_watch_ws(
    websocket: WebSocket,
    settings: Settings,
    ai: AIService,
    tts: TTSService,
    asr: ASRService,
) -> None:
    if not await check_watch_ws(websocket, settings):
        logger.warning("watch AI websocket rejected (bad token)")
        return
    await websocket.accept()
    logger.info(
        "watch AI websocket connected: client=%s provider=%s",
        websocket.client.host if websocket.client else "?",
        websocket.query_params.get("provider") or "deepseek",
    )
    session = WatchSession(websocket, settings, ai, tts, asr)
    await session.run()
    logger.info("watch AI websocket closed")


class WatchSession:
    def __init__(
        self,
        websocket: WebSocket,
        settings: Settings,
        ai: AIService,
        tts: TTSService,
        asr: ASRService,
    ) -> None:
        self.websocket = websocket
        self.settings = settings
        self.ai = ai
        self.tts = tts
        self.asr = asr
        self.session_id = uuid.uuid4().hex
        self.provider = websocket.query_params.get("provider") or "deepseek"
        self.model = websocket.query_params.get("model") or ""
        self.audio_packets: list[bytes] = []
        self.audio_bytes = 0
        self.input_sample_rate = 16000
        self.listening = False
        self.closed = False
        self._send_lock = asyncio.Lock()
        self._task: asyncio.Task[Any] | None = None
        self._ptt_timer: asyncio.Task[Any] | None = None
        self._ptt_done = False
        self._history: list[dict[str, str]] = []
        self._hello_sent = False

    async def run(self) -> None:
        await self._send_hello()
        try:
            while True:
                message = await self.websocket.receive()
                if message.get("type") == "websocket.disconnect":
                    break
                if message.get("text") is not None:
                    logger.info("watch message: %s", message["text"][:240])
                    await self._on_text(message["text"])
                elif message.get("bytes") is not None:
                    self._on_audio(message["bytes"])
        except WebSocketDisconnect:
            pass
        except Exception as exc:  # noqa: BLE001 - a bad client must not kill the server
            logger.warning("watch websocket error: %s", exc)
        finally:
            self.closed = True
            await self._cancel_ptt_timer()
            await self._cancel_task()

    async def _send_hello(self) -> None:
        if self._hello_sent:
            return
        self._hello_sent = True
        await self._send({
            "type": "hello",
            "transport": "websocket",
            "session_id": self.session_id,
            "audio_params": {
                "format": "opus",
                "sample_rate": self.settings.watch_ai_sample_rate,
                "channels": 1,
                "frame_duration": self.settings.watch_ai_frame_ms,
            },
        })

    async def _on_text(self, text: str) -> None:
        try:
            message = json.loads(text)
        except json.JSONDecodeError:
            await self._error("bad JSON")
            return
        if not isinstance(message, dict):
            await self._error("bad message")
            return
        kind = str(message.get("type", "")).lower()
        if kind == "hello":
            audio = message.get("audio_params")
            if isinstance(audio, dict):
                try:
                    rate = int(audio.get("sample_rate") or 0)
                except (TypeError, ValueError):
                    rate = 0
                if 8000 <= rate <= 48000:
                    self.input_sample_rate = rate
            await self._send_hello()
            return
        if kind == "listen":
            state = str(message.get("state", "")).lower()
            if state == "start":
                await self._cancel_ptt_timer()
                await self._cancel_task()
                self.audio_packets = []
                self.audio_bytes = 0
                self.listening = True
                self._ptt_done = False
                logger.info("watch PTT start")
                self._ptt_timer = asyncio.create_task(self._ptt_timeout())
                return
            if state == "stop":
                if self._ptt_done:
                    logger.info("watch PTT late stop ignored")
                    return
                self._ptt_done = True
                await self._cancel_ptt_timer()
                self.listening = False
                direct_text = str(message.get("text", "")).strip()
                if direct_text:
                    logger.info("watch PTT stop: direct text")
                    self._start(self._process_text(direct_text))
                elif self.audio_packets:
                    packets = list(self.audio_packets)
                    packet_bytes = self.audio_bytes
                    self.audio_packets = []
                    self.audio_bytes = 0
                    logger.info(
                        "watch PTT stop: %d Opus packets / %d bytes",
                        len(packets),
                        packet_bytes,
                    )
                    self._start(self._process_audio(packets))
                else:
                    logger.info("watch PTT stop: no audio packets")
                    self._start(self._no_speech("no audio"))
                return
            return
        if kind == "ping":
            # Cheap round-trip probe: no ASR, AI or TTS involved.
            await self._send({"type": "pong", "session_id": self.session_id})
            return
        if kind in {"chat", "text"}:
            prompt = str(message.get("text") or message.get("content") or "").strip()
            if prompt:
                self._start(self._process_text(prompt))
            return
        if kind == "abort":
            await self._cancel_task()
            await self._send({"type": "tts", "state": "stop"})
            return
        if kind == "mcp":
            logger.info("MCP message ignored")
            return
        logger.debug("unhandled watch message type: %s", kind)

    def _on_audio(self, data: bytes) -> None:
        if not self.listening or not data:
            return
        self.audio_packets.append(data)
        self.audio_bytes += len(data)
        if self.audio_bytes > 8 * 1024 * 1024 or len(self.audio_packets) > 2000:
            self.audio_packets = []
            self.audio_bytes = 0
            self.listening = False
            asyncio.create_task(self._error("audio buffer too large"))

    def _start(self, coroutine: Any) -> None:
        if self._task and not self._task.done():
            self._task.cancel()
        self._task = asyncio.create_task(self._guard(coroutine))

    async def _guard(self, coroutine: Any) -> None:
        try:
            await coroutine
        except asyncio.CancelledError:
            raise
        except (AIError, ASRError, TTSError, AudioError) as exc:
            await self._error(str(exc))
        except Exception as exc:  # noqa: BLE001
            logger.exception("watch AI task failed")
            await self._error("server error: %s" % str(exc)[:160])

    async def _cancel_task(self) -> None:
        task = self._task
        self._task = None
        if task and not task.done():
            task.cancel()
            try:
                await task
            except (asyncio.CancelledError, Exception):
                pass

    async def _cancel_ptt_timer(self) -> None:
        task = self._ptt_timer
        self._ptt_timer = None
        if task and not task.done():
            task.cancel()
            try:
                await task
            except (asyncio.CancelledError, Exception):
                pass

    async def _ptt_timeout(self) -> None:
        try:
            await asyncio.sleep(max(1.0, self.settings.ptt_max_s))
        except asyncio.CancelledError:
            return
        if self.closed or not self.listening:
            return
        self.listening = False
        self._ptt_done = True
        self._ptt_timer = None
        packets = list(self.audio_packets)
        packet_bytes = self.audio_bytes
        self.audio_packets = []
        self.audio_bytes = 0
        logger.warning(
            "watch PTT no stop after %.0fs (%d packets / %d bytes); auto-finishing",
            self.settings.ptt_max_s,
            len(packets),
            packet_bytes,
        )
        if packets:
            self._start(self._process_audio(packets))
        else:
            self._start(self._no_speech("PTT timeout"))

    async def _process_audio(self, packets: list[bytes]) -> None:
        started = time.monotonic()
        try:
            text = await asyncio.wait_for(
                self.asr.transcribe_opus(
                    packets, source_rate=self.input_sample_rate
                ),
                timeout=max(5.0, self.settings.asr_timeout_s + 5.0),
            )
        except asyncio.TimeoutError:
            logger.warning("watch ASR timeout after %.1fs", time.monotonic() - started)
            await self._error("ASR timeout")
            return
        except ASRError as exc:
            logger.warning("watch ASR failed: %s", exc)
            await self._error(str(exc))
            return
        text = text.strip()
        elapsed = time.monotonic() - started
        if not text:
            logger.info("watch ASR empty after %.2fs", elapsed)
            await self._no_speech("empty ASR")
            return
        logger.info("watch ASR: %.2fs, %d chars: %s", elapsed, len(text), truncate_utf8(text, 120))
        await self._process_text(text)

    async def _no_speech(self, reason: str) -> None:
        prompt = (self.settings.empty_asr_prompt or "").strip()
        if not prompt:
            prompt = "用户刚刚回复了空消息，请用简短中文提示他再说一遍。"
        logger.info("watch no-speech prompt (%s): %s", reason, truncate_utf8(prompt, 180))
        await self._process_text(
            prompt,
            display_text="（未检测到语音）",
            remember=False,
        )

    async def _process_text(
        self,
        text: str,
        *,
        display_text: str | None = None,
        remember: bool = True,
    ) -> None:
        text = text.strip()[:2000]
        if not text:
            await self._error("empty prompt")
            return
        logger.info("watch LLM start: %s", truncate_utf8(text, 120))
        await self._send({
            "type": "stt",
            "session_id": self.session_id,
            "text": truncate_utf8(display_text if display_text is not None else text, 120),
        })
        messages: list[dict[str, str]] = [
            {"role": "system", "content": self.settings.watch_system_prompt}
        ]
        messages.extend(self._history[-16:])
        messages.append({"role": "user", "content": text})
        chunks: list[str] = []
        sent_display = ""
        last_stream_send = 0.0
        await self._send({
            "type": "llm",
            "session_id": self.session_id,
            "state": "start",
            "emotion": "neutral",
            "text": "",
        })
        try:
            async for chunk in self.ai.stream(
                messages,
                provider=self.provider,
                model=self.model or None,
                max_tokens=self.settings.ai_max_tokens,
            ):
                chunks.append(chunk)
                now = time.monotonic()
                streamed = "".join(chunks)
                display_now = streamed
                if display_now and display_now != sent_display and (
                    not sent_display or now - last_stream_send >= 0.08
                ):
                    sent_display = display_now
                    last_stream_send = now
                    await self._send({
                        "type": "llm",
                        "session_id": self.session_id,
                        "state": "delta",
                        "emotion": "neutral",
                        "text": display_now,
                    })
        except AIError as exc:
            await self._error(str(exc))
            return
        reply = "".join(chunks).strip()
        if not reply:
            await self._error("AI returned an empty reply")
            return
        logger.info("watch LLM reply: %d chars", len(reply))
        await self._send({
            "type": "llm",
            "session_id": self.session_id,
            "state": "done",
            "emotion": "neutral",
            "text": reply,
        })
        if remember:
            self._history.extend([
                {"role": "user", "content": text[:1000]},
                {"role": "assistant", "content": reply[:4000]},
            ])
            self._history = self._history[-16:]
        await self._speak(reply)


    async def _speak(self, text: str) -> None:
        tts_text = text.strip()
        if not tts_text:
            return
        try:
            wav = await asyncio.to_thread(self.tts.synthesize, tts_text, None)
            pcm = await asyncio.to_thread(
                wav_to_pcm16, wav, self.settings.watch_ai_sample_rate
            )
            frames = await asyncio.to_thread(
                pcm16_to_opus_frames,
                pcm,
                self.settings.watch_ai_sample_rate,
                self.settings.watch_ai_frame_ms,
            )
        except (TTSError, AudioError) as exc:
            logger.warning("watch TTS unavailable: %s", exc)
            await self._send({"type": "tts", "state": "start"})
            await self._send({"type": "tts", "state": "stop"})
            return
        logger.info("watch TTS start: %d chars", len(tts_text))
        await self._send({"type": "tts", "state": "start"})
        await self._send({
            "type": "tts",
            "state": "sentence_start",
            "text": truncate_utf8(tts_text, 180),
        })
        logger.info(
            "watch TTS: %d Opus frames at %d Hz / %d ms",
            len(frames),
            self.settings.watch_ai_sample_rate,
            self.settings.watch_ai_frame_ms,
        )
        interval = self.settings.watch_ai_frame_ms / 1000.0
        for frame in frames:
            if self.closed:
                break
            try:
                async with self._send_lock:
                    await self.websocket.send_bytes(frame)
            except (RuntimeError, WebSocketDisconnect):
                self.closed = True
                break
            await asyncio.sleep(interval)
        await asyncio.sleep(0.15)
        await self._send({"type": "tts", "state": "stop"})
        logger.info("watch TTS stop")

    async def _send(self, payload: dict[str, Any]) -> None:
        if self.closed:
            return
        try:
            async with self._send_lock:
                await self.websocket.send_text(
                    json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
                )
        except (RuntimeError, WebSocketDisconnect):
            self.closed = True

    async def _error(self, message: str) -> None:
        logger.info("watch error: %s", message)
        await self._send({"type": "error", "message": message[:240]})
