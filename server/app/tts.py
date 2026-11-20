"""TTS providers: local Piper (default) and an optional CosyVoice HTTP adapter."""

from __future__ import annotations

import base64
import hashlib
import io
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import wave
from pathlib import Path
from typing import Any

import httpx

from .config import Settings


class TTSError(RuntimeError):
    def __init__(self, message: str, status_code: int = 503) -> None:
        super().__init__(message)
        self.status_code = status_code


class TTSService:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self._piper_lock = threading.Lock()
        self._voice: Any = None
        self._voice_key = ""

    def _piper_models(self) -> list[Path]:
        directory = self.settings.piper_model_dir
        if not directory or not directory.exists():
            return []
        return sorted(path for path in directory.glob("*.onnx") if path.is_file())

    def _find_piper_model(self, voice: str | None = None) -> Path | None:
        explicit = Path(self.settings.piper_model).expanduser() if self.settings.piper_model else None
        if explicit and explicit.is_file():
            return explicit
        requested = (voice or self.settings.piper_voice or "").strip()
        models = self._piper_models()
        if requested:
            safe_name = Path(requested).name
            for path in models:
                if path.name == safe_name or path.stem == safe_name:
                    return path
            candidate = self.settings.piper_model_dir / (safe_name + ".onnx")
            if candidate.is_file():
                return candidate
        return models[0] if models else None

    def _cache_path(self, provider: str, voice: str, text: str) -> Path:
        digest = hashlib.sha256(("%s\0%s\0%s" % (provider, voice, text)).encode("utf-8")).hexdigest()
        return self.settings.tts_cache_dir / (digest + ".wav")

    def _validate_wav(self, data: bytes) -> bytes:
        if len(data) < 12 or not data.startswith(b"RIFF") or data[8:12] != b"WAVE":
            raise TTSError("TTS provider did not return a WAV file")
        return data

    def voices(self) -> list[dict[str, str]]:
        provider = self.settings.tts_provider.lower()
        if provider == "cosyvoice":
            return [{"id": self.settings.cosyvoice_speaker, "name": self.settings.cosyvoice_speaker}]
        if provider == "piper":
            models = self._piper_models()
            if models:
                return [{"id": path.stem, "name": path.stem} for path in models]
            if self.settings.piper_model:
                return [{"id": Path(self.settings.piper_model).stem, "name": Path(self.settings.piper_model).stem}]
        return []

    def status(self) -> dict[str, Any]:
        provider = self.settings.tts_provider.lower()
        detail = ""
        available = False
        if provider == "piper":
            if self.settings.piper_url:
                available = True
                detail = self.settings.piper_url
            elif self._find_piper_model():
                available = True
                detail = "local Piper model found"
            else:
                detail = "no Piper model; run scripts/setup_tts.sh or set PIPER_MODEL"
        elif provider == "cosyvoice":
            available = bool(self.settings.cosyvoice_url)
            detail = self.settings.cosyvoice_url or "COSYVOICE_URL is empty"
        else:
            detail = "TTS_PROVIDER=none"
        return {
            "provider": provider,
            "available": available,
            "detail": detail,
            "voice": self.settings.piper_voice if provider == "piper" else self.settings.cosyvoice_speaker,
            "voices": self.voices(),
        }

    def synthesize(self, text: str, voice: str | None = None) -> bytes:
        text = (text or "").strip()
        if not text:
            raise TTSError("text is empty", 400)
        provider = self.settings.tts_provider.lower()
        selected_voice = (voice or "").strip()
        if provider == "piper":
            model = self._find_piper_model(selected_voice)
            cache_voice = model.stem if model else self.settings.piper_voice
        elif provider == "cosyvoice":
            cache_voice = selected_voice or self.settings.cosyvoice_speaker
        else:
            raise TTSError("TTS is disabled (TTS_PROVIDER=none)")

        cache_path = self._cache_path(provider, cache_voice, text)
        if self.settings.tts_cache and cache_path.is_file():
            data = cache_path.read_bytes()
            if data:
                return data

        if provider == "piper":
            data = self._synthesize_piper(text, selected_voice)
        elif provider == "cosyvoice":
            data = self._synthesize_cosyvoice(text, selected_voice)
        else:
            raise TTSError("unsupported TTS provider: %s" % provider)
        data = self._validate_wav(data)
        if self.settings.tts_cache:
            self.settings.tts_cache_dir.mkdir(parents=True, exist_ok=True)
            try:
                cache_path.write_bytes(data)
            except OSError:
                pass
        return data

    def _synthesize_piper(self, text: str, voice: str | None) -> bytes:
        if self.settings.piper_url:
            payload: dict[str, Any] = {"text": text}
            if voice:
                payload["voice"] = voice
            url = self.settings.piper_url.rstrip("/") + "/synthesize"
            try:
                with httpx.Client(timeout=self.settings.tts_timeout_s) as client:
                    response = client.post(url, json=payload)
                    if response.status_code != 200:
                        raise TTSError("Piper HTTP %d: %s" % (response.status_code, response.text[:200]))
                    return response.content
            except TTSError:
                raise
            except httpx.HTTPError as exc:
                raise TTSError("Piper HTTP request failed: %s" % exc) from exc

        model = self._find_piper_model(voice)
        if not model:
            raise TTSError("Piper model not found; run scripts/setup_tts.sh")
        try:
            from piper import PiperVoice  # type: ignore
        except ImportError:
            return self._synthesize_piper_cli(text, model)

        with self._piper_lock:
            if self._voice is None or self._voice_key != str(model):
                self._voice = PiperVoice.load(str(model))
                self._voice_key = str(model)
            output = io.BytesIO()
            with wave.open(output, "wb") as wav_file:
                self._voice.synthesize_wav(text, wav_file)
        return output.getvalue()

    def _synthesize_piper_cli(self, text: str, model: Path) -> bytes:
        command = shlex.split(self.settings.piper_bin or "piper")
        if not command:
            command = ["piper"]
        if shutil.which(command[0]) is None:
            command = [sys.executable, "-m", "piper"]
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as temp:
            output_path = Path(temp.name)
        command += ["-m", str(model), "-f", str(output_path), "--", text]
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=self.settings.tts_timeout_s,
                check=False,
            )
            if result.returncode != 0:
                raise TTSError("Piper failed: %s" % (result.stderr or result.stdout or "unknown error")[:300])
            data = output_path.read_bytes()
            return self._validate_wav(data)
        except subprocess.TimeoutExpired as exc:
            raise TTSError("Piper timed out") from exc
        finally:
            try:
                output_path.unlink()
            except OSError:
                pass


    def _synthesize_cosyvoice(self, text: str, voice: str | None) -> bytes:
        if not self.settings.cosyvoice_url:
            raise TTSError("COSYVOICE_URL is empty")
        payload = {
            "text": text,
            "speaker": voice or self.settings.cosyvoice_speaker,
            "format": "wav",
        }
        headers = {"Content-Type": "application/json"}
        if self.settings.cosyvoice_token:
            headers["Authorization"] = "Bearer " + self.settings.cosyvoice_token
        try:
            with httpx.Client(timeout=self.settings.tts_timeout_s) as client:
                response = client.post(self.settings.cosyvoice_url, json=payload, headers=headers)
                if response.status_code != 200:
                    raise TTSError("CosyVoice HTTP %d: %s" % (response.status_code, response.text[:200]))
                content_type = response.headers.get("content-type", "")
                if "json" in content_type:
                    data = response.json()
                    audio = data.get("audio") or data.get("data") or ""
                    if isinstance(audio, str) and audio:
                        if audio.startswith("data:"):
                            audio = audio.split(",", 1)[-1]
                        return base64.b64decode(audio)
                    raise TTSError("CosyVoice JSON response has no audio field")
                return response.content
        except TTSError:
            raise
        except (httpx.HTTPError, ValueError) as exc:
            raise TTSError("CosyVoice request failed: %s" % exc) from exc
