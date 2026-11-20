"""Optional speech-to-text adapter for the watch's Opus upload."""

from __future__ import annotations

import asyncio
import logging
import struct
from collections.abc import Iterable
from typing import Any

import httpx

from .audio import AudioError, opus_packets_to_pcm16, pcm16_to_wav
from .config import Settings

logger = logging.getLogger("uvicorn.error")


class ASRError(RuntimeError):
    def __init__(self, message: str, status_code: int = 503) -> None:
        super().__init__(message)
        self.status_code = status_code


class ASRService:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings

    def status(self) -> dict[str, Any]:
        provider = self.settings.asr_provider.lower()
        configured = provider in {"openai_compatible", "openai"} and bool(self.settings.asr_base_url)
        if configured and not self.settings.asr_api_key:
            detail = "local/OpenAI-compatible endpoint (no API key)"
        elif configured:
            detail = "API endpoint configured"
        elif provider == "none":
            detail = "set ASR_PROVIDER=openai_compatible and ASR_BASE_URL"
        else:
            detail = "ASR_BASE_URL is empty"
        return {
            "provider": provider,
            "available": configured,
            "model": self.settings.asr_model,
            "language": self.settings.asr_language,
            "detail": detail,
        }

    async def transcribe_opus(self, packets: Iterable[bytes], source_rate: int = 24000) -> str:
        provider = self.settings.asr_provider.lower()
        if provider not in {"openai_compatible", "openai"}:
            raise ASRError(
                "ASR is not configured; set ASR_PROVIDER=openai_compatible "
                "and ASR_BASE_URL"
            )
        if not self.settings.asr_base_url:
            raise ASRError("ASR_BASE_URL is empty")
        if not self.settings.asr_model:
            raise ASRError("ASR_MODEL is empty")
        try:
            pcm = await asyncio.to_thread(
                opus_packets_to_pcm16, list(packets), target_rate=16000, source_rate=source_rate
            )
        except AudioError as exc:
            raise ASRError(str(exc)) from exc
        if not pcm:
            raise ASRError("no decodable audio received")
        sample_count = len(pcm) // 2
        if sample_count:
            total = 0
            peak = 0
            for (value,) in struct.iter_unpack("<h", pcm):
                total += value * value
                magnitude = -value if value < 0 else value
                if magnitude > peak:
                    peak = magnitude
            rms = int((total / sample_count) ** 0.5)
            logger.info(
                "watch ASR PCM: %.2fs rms=%d peak=%d",
                sample_count / 16000.0,
                rms,
                peak,
            )
            if peak < 30:
                logger.warning("watch ASR PCM is silent (peak=%d)", peak)
        wav = pcm16_to_wav(pcm, 16000)
        base = self.settings.asr_base_url.rstrip("/")
        url = base if base.endswith("/audio/transcriptions") else base + "/audio/transcriptions"
        headers = {}
        if self.settings.asr_api_key:
            headers["Authorization"] = "Bearer " + self.settings.asr_api_key
        data = {"model": self.settings.asr_model, "response_format": "json"}
        if self.settings.asr_language:
            data["language"] = self.settings.asr_language
        if self.settings.asr_prompt:
            data["prompt"] = self.settings.asr_prompt
        timeout = httpx.Timeout(self.settings.asr_timeout_s, connect=8.0)
        try:
            async with httpx.AsyncClient(timeout=timeout) as client:
                response = await client.post(
                    url,
                    headers=headers,
                    data=data,
                    files={"file": ("speech.wav", wav, "audio/wav")},
                )
            if response.status_code < 200 or response.status_code >= 300:
                raise ASRError("ASR HTTP %d: %s" % (response.status_code, response.text[:300]))
            payload = response.json()
            text = str(payload.get("text") or payload.get("transcript") or "").strip()
            return text
        except ASRError:
            raise
        except (httpx.HTTPError, ValueError) as exc:
            raise ASRError("ASR request failed: %s" % exc) from exc
