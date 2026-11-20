from __future__ import annotations

import asyncio
from pathlib import Path

import app.asr as asr_module
from app.asr import ASRService
from app.config import Settings


class FakeResponse:
    status_code = 200
    text = ""

    def json(self):
        return {"text": "你好"}


def test_asr_openai_compatible_request(tmp_path: Path, monkeypatch):
    seen = {}

    class FakeClient:
        def __init__(self, *args, **kwargs):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, *args):
            return False

        async def post(self, url, headers=None, data=None, files=None):
            seen["url"] = url
            seen["headers"] = headers
            seen["data"] = data
            seen["files"] = files
            return FakeResponse()

    monkeypatch.setattr(asr_module.httpx, "AsyncClient", FakeClient)
    monkeypatch.setattr(
        asr_module,
        "opus_packets_to_pcm16",
        lambda packets, target_rate=16000, source_rate=24000: b"\x00\x00" * 160,
    )
    monkeypatch.setattr(asr_module, "pcm16_to_wav", lambda pcm, rate: b"RIFF....")

    settings = Settings(
        data_dir=tmp_path,
        asr_provider="openai_compatible",
        asr_base_url="https://asr.example.test/v1",
        asr_api_key="asr-key",
        asr_model="whisper-1",
        asr_language="zh",
        asr_prompt="ESP32",
    )
    text = asyncio.run(ASRService(settings).transcribe_opus([b"opus"], source_rate=24000))

    assert text == "你好"
    assert seen["url"] == "https://asr.example.test/v1/audio/transcriptions"
    assert seen["headers"]["Authorization"] == "Bearer asr-key"
    assert seen["data"]["model"] == "whisper-1"
    assert seen["data"]["language"] == "zh"
    assert seen["data"]["prompt"] == "ESP32"
    assert seen["files"]["file"][0] == "speech.wav"


def test_asr_empty_response_returns_empty_text(tmp_path: Path, monkeypatch):
    class EmptyResponse:
        status_code = 200
        text = ""

        def json(self):
            return {"text": ""}

    class FakeClient:
        def __init__(self, *args, **kwargs):
            pass

        async def __aenter__(self):
            return self

        async def __aexit__(self, *args):
            return False

        async def post(self, *args, **kwargs):
            return EmptyResponse()

    monkeypatch.setattr(asr_module.httpx, "AsyncClient", FakeClient)
    monkeypatch.setattr(
        asr_module,
        "opus_packets_to_pcm16",
        lambda packets, target_rate=16000, source_rate=24000: b"\x00\x00" * 160,
    )
    monkeypatch.setattr(asr_module, "pcm16_to_wav", lambda pcm, rate: b"RIFF....")

    settings = Settings(
        data_dir=tmp_path,
        asr_provider="openai_compatible",
        asr_base_url="https://asr.example.test/v1",
        asr_model="whisper-1",
    )
    text = asyncio.run(ASRService(settings).transcribe_opus([b"opus"], source_rate=24000))
    assert text == ""
