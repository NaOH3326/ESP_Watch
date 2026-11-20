"""TTS providers: local Piper (default) and an optional CosyVoice HTTP adapter."""

from __future__ import annotations

import base64
import hashlib
import io
import json
import os
import re
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
        self._active_voice_file = self.settings.data_dir / "tts-voice.txt"

    @staticmethod
    def _safe_voice_name(value: str) -> str:
        """Return a path-safe Piper voice stem.

        Piper voice files are usually named like
        ``zh_CN-huayan-medium.onnx``.  The Dashboard lets users keep that
        name, but never allows a path separator or an empty / dot-only stem.
        """
        raw = Path(str(value or "")).name.strip()
        for suffix in (".onnx.json", ".onnx"):
            if raw.lower().endswith(suffix):
                raw = raw[: -len(suffix)]
                break
        raw = re.sub(r"[^A-Za-z0-9._-]+", "-", raw).strip(".-")
        return raw[:80]

    def _piper_models(self) -> list[Path]:
        directory = self.settings.piper_model_dir
        if not directory or not directory.exists():
            return []
        return sorted(path for path in directory.glob("*.onnx") if path.is_file())

    @staticmethod
    def _config_for(model: Path) -> Path:
        """Piper looks for ``<voice>.onnx.json`` next to the model."""
        return model.with_name(model.name + ".json")

    @staticmethod
    def _alternate_config_for(model: Path) -> Path:
        return model.with_suffix(".json")

    def _model_by_name(self, voice: str | None) -> Path | None:
        requested = self._safe_voice_name(voice or "")
        if not requested:
            return None
        for path in self._piper_models():
            if path.stem == requested or path.name == requested:
                return path
        candidate = self.settings.piper_model_dir / (requested + ".onnx")
        return candidate if candidate.is_file() else None

    def _active_voice_name(self) -> str:
        try:
            requested = self._active_voice_file.read_text(encoding="utf-8").strip()
        except OSError:
            requested = ""
        model = self._model_by_name(requested)
        return model.stem if model else ""

    def _write_active_voice(self, voice: str | None) -> None:
        self.settings.data_dir.mkdir(parents=True, exist_ok=True)
        temp = self._active_voice_file.with_name("." + self._active_voice_file.name + ".part")
        if voice:
            temp.write_text(voice + "\n", encoding="utf-8")
            os.replace(temp, self._active_voice_file)
        else:
            try:
                self._active_voice_file.unlink()
            except FileNotFoundError:
                pass
            try:
                temp.unlink()
            except FileNotFoundError:
                pass

    def _model_has_config(self, model: Path) -> bool:
        return self._config_for(model).is_file() or self._alternate_config_for(model).is_file()

    def _voice_info(self, model: Path) -> dict[str, Any]:
        try:
            size = model.stat().st_size
        except OSError:
            size = 0
        return {
            "id": model.stem,
            "name": model.stem,
            "size": size,
            "has_config": self._model_has_config(model),
            "active": model.stem == self._active_voice_name(),
        }

    def _find_piper_model(self, voice: str | None = None) -> Path | None:
        explicit = Path(self.settings.piper_model).expanduser() if self.settings.piper_model else None
        if explicit and explicit.is_file():
            return explicit
        requested = (voice or self._active_voice_name() or self.settings.piper_voice or "").strip()
        models = self._piper_models()
        if requested:
            selected = self._model_by_name(requested)
            if selected:
                return selected
        return models[0] if models else None

    async def _save_upload(self, upload: Any, target: Path, max_bytes: int) -> tuple[Path, int]:
        """Stream an UploadFile to a temporary file with a hard size limit."""
        temp = target.with_name("." + target.name + ".part")
        size = 0
        try:
            with temp.open("wb") as output:
                while True:
                    chunk = await upload.read(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    if size > max_bytes:
                        raise TTSError(
                            "file is larger than %d MB" % self.settings.max_tts_model_mb,
                            413,
                        )
                    output.write(chunk)
            if size == 0:
                raise TTSError("uploaded model file is empty", 400)
            return temp, size
        except Exception:
            try:
                temp.unlink()
            except OSError:
                pass
            raise
        finally:
            try:
                await upload.close()
            except Exception:
                pass

    def _cache_path(self, provider: str, voice: str, text: str) -> Path:
        digest = hashlib.sha256(("%s\0%s\0%s" % (provider, voice, text)).encode("utf-8")).hexdigest()
        return self.settings.tts_cache_dir / (digest + ".wav")

    def _validate_wav(self, data: bytes) -> bytes:
        if len(data) < 12 or not data.startswith(b"RIFF") or data[8:12] != b"WAVE":
            raise TTSError("TTS provider did not return a WAV file")
        return data

    def voices(self) -> list[dict[str, Any]]:
        provider = self.settings.tts_provider.lower()
        if provider == "cosyvoice":
            return [{"id": self.settings.cosyvoice_speaker, "name": self.settings.cosyvoice_speaker}]
        if provider == "piper":
            models = self._piper_models()
            if models:
                return [self._voice_info(path) for path in models]
            if self.settings.piper_model:
                explicit = Path(self.settings.piper_model).expanduser()
                if explicit.is_file():
                    return [self._voice_info(explicit)]
        return []

    def status(self) -> dict[str, Any]:
        provider = self.settings.tts_provider.lower()
        detail = ""
        available = False
        active_voice = self._active_voice_name() if provider == "piper" else ""
        if provider == "piper":
            if self.settings.piper_url:
                available = True
                detail = self.settings.piper_url
            elif self._find_piper_model():
                model = self._find_piper_model()
                if model and not self._model_has_config(model):
                    detail = "Piper model exists but .onnx.json config is missing"
                else:
                    available = True
                    detail = "local Piper model found"
            else:
                detail = "no Piper model; upload one in Dashboard or run scripts/setup_tts.sh"
        elif provider == "cosyvoice":
            available = bool(self.settings.cosyvoice_url)
            detail = self.settings.cosyvoice_url or "COSYVOICE_URL is empty"
        else:
            detail = "TTS_PROVIDER=none"
        return {
            "provider": provider,
            "available": available,
            "detail": detail,
            "voice": (active_voice or self.settings.piper_voice)
            if provider == "piper"
            else self.settings.cosyvoice_speaker,
            "voices": self.voices(),
        }

    async def upload_voice(
        self,
        model: Any,
        config: Any | None = None,
        *,
        name: str = "",
        activate: bool = True,
        overwrite: bool = False,
    ) -> dict[str, Any]:
        """Store an uploaded Piper ``.onnx`` and optional ``.onnx.json``.

        The upload is streamed to disk, so a 60–200 MB voice does not have to
        sit in RAM.  A matching JSON config is required for a new voice;
        replacing an existing model may omit it and keep the old config.
        """
        if self.settings.tts_provider.lower() != "piper":
            raise TTSError("model upload is only available for TTS_PROVIDER=piper", 400)
        filename = Path(str(getattr(model, "filename", "") or "")).name
        if not filename.lower().endswith(".onnx"):
            raise TTSError("model file must end with .onnx", 400)
        voice_name = self._safe_voice_name(name or filename)
        if not voice_name:
            raise TTSError("invalid voice name", 400)

        directory = self.settings.piper_model_dir
        directory.mkdir(parents=True, exist_ok=True)
        model_path = directory / (voice_name + ".onnx")
        config_path = self._config_for(model_path)
        if model_path.exists() and not overwrite:
            raise TTSError("voice already exists; rename it or enable overwrite", 409)
        if config is None and not self._model_has_config(model_path):
            raise TTSError("missing Piper config; upload a matching .onnx.json", 400)

        model_temp: Path | None = None
        config_temp: Path | None = None
        try:
            model_temp, _ = await self._save_upload(model, model_path, self.settings.max_tts_model_bytes)
            if config is not None:
                config_filename = Path(str(getattr(config, "filename", "") or "")).name
                if not config_filename.lower().endswith(".json"):
                    raise TTSError("config file must end with .json", 400)
                config_temp, _ = await self._save_upload(
                    config,
                    config_path,
                    min(self.settings.max_tts_model_bytes, 8 * 1024 * 1024),
                )
                try:
                    parsed = json.loads(config_temp.read_text(encoding="utf-8"))
                except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
                    raise TTSError("Piper config is not valid JSON", 400) from exc
                if not isinstance(parsed, dict):
                    raise TTSError("Piper config must be a JSON object", 400)

            if config_temp is not None:
                os.replace(config_temp, config_path)
            os.replace(model_temp, model_path)
        except Exception:
            for temp in (model_temp, config_temp):
                if temp is not None:
                    try:
                        temp.unlink()
                    except OSError:
                        pass
            raise
        finally:
            if config is not None:
                try:
                    await config.close()
                except Exception:
                    pass

        with self._piper_lock:
            self._voice = None
            self._voice_key = ""
        if activate:
            self._write_active_voice(voice_name)
        return self._voice_info(model_path)

    def select_voice(self, voice: str) -> dict[str, Any]:
        if self.settings.tts_provider.lower() != "piper":
            raise TTSError("voice selection is only available for TTS_PROVIDER=piper", 400)
        model = self._model_by_name(voice)
        if not model:
            raise TTSError("voice not found", 404)
        self._write_active_voice(model.stem)
        with self._piper_lock:
            self._voice = None
            self._voice_key = ""
        return self._voice_info(model)

    def delete_voice(self, voice: str) -> dict[str, Any]:
        if self.settings.tts_provider.lower() != "piper":
            raise TTSError("voice deletion is only available for TTS_PROVIDER=piper", 400)
        model = self._model_by_name(voice)
        if not model:
            raise TTSError("voice not found", 404)
        was_active = model.stem == self._active_voice_name()
        for path in (model, self._config_for(model), self._alternate_config_for(model)):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
            except OSError as exc:
                raise TTSError("could not delete voice file: %s" % exc, 500) from exc
        if was_active:
            self._write_active_voice(None)
        with self._piper_lock:
            self._voice = None
            self._voice_key = ""
        return {"deleted": model.stem, "active": self._active_voice_name()}

    def synthesize(self, text: str, voice: str | None = None) -> bytes:
        text = (text or "").strip()
        if not text:
            raise TTSError("text is empty", 400)
        provider = self.settings.tts_provider.lower()
        selected_voice = (voice or "").strip()
        if provider == "piper":
            model = self._find_piper_model(selected_voice)
            if model:
                try:
                    cache_voice = "%s@%d" % (model.stem, model.stat().st_mtime_ns)
                except OSError:
                    cache_voice = model.stem
            else:
                cache_voice = self.settings.piper_voice
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
        config = self._config_for(model)
        if not config.is_file() and self._alternate_config_for(model).is_file():
            config = self._alternate_config_for(model)
        if not config.is_file():
            raise TTSError(
                "Piper model is missing %s; upload the matching .onnx.json"
                % self._config_for(model).name
            )
        try:
            from piper import PiperVoice  # type: ignore
        except ImportError:
            return self._synthesize_piper_cli(text, model)

        with self._piper_lock:
            if self._voice is None or self._voice_key != str(model):
                self._voice = PiperVoice.load(str(model), config_path=str(config))
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
