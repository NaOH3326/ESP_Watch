from __future__ import annotations

import json
from pathlib import Path

import pytest

from app.audio import pcm16_to_wav
from app.config import Settings
from app.main import create_app

pytest.importorskip("av")


class FakeAI:
    def __init__(self) -> None:
        self.messages = []

    async def stream(self, messages, **kwargs):
        self.messages = messages
        for chunk in ("你好", "，我是测试助手。"):
            yield chunk

    async def provider_status(self):
        return {"default": "deepseek", "providers": {"deepseek": {"available": True}}}

    def resolve_provider(self, provider=None):
        return "deepseek"


class FakeTTS:
    def synthesize(self, text, voice=None):
        rate = 24000
        pcm = b"\x00\x00" * (rate // 20)  # 50 ms
        return pcm16_to_wav(pcm, rate)

    def status(self):
        return {"provider": "fake", "available": True, "voices": []}

    def voices(self):
        return []


class NoASR:
    async def transcribe_opus(self, packets, source_rate=24000):
        raise AssertionError("ASR must not be called for text input")


class LongAI(FakeAI):
    async def stream(self, messages, **kwargs):
        self.messages = messages
        for _ in range(4):
            yield "长回复" * 20


def test_watch_ws_long_reply_is_not_truncated(tmp_path: Path):
    settings = Settings(
        data_dir=tmp_path,
        watch_token="watch-token",
        tts_provider="piper",
    )
    settings.ensure_dirs()
    app = create_app(settings, ai_service=LongAI(), tts_service=FakeTTS(), asr_service=NoASR())
    from fastapi.testclient import TestClient

    with TestClient(app) as client:
        with client.websocket_connect(
            "/v1/ws", headers={"Authorization": "Bearer watch-token"}
        ) as websocket:
            assert websocket.receive_json()["type"] == "hello"
            websocket.send_json({"type": "chat", "text": "请详细回答"})
            done_text = ""
            while True:
                message = websocket.receive()
                if message.get("type") == "websocket.close":
                    break
                if message.get("bytes") is not None:
                    continue
                payload = json.loads(message["text"])
                if payload["type"] == "llm" and payload.get("state") == "done":
                    done_text = payload["text"]
                if payload["type"] == "tts" and payload.get("state") == "stop":
                    break
            assert len(done_text) == len("长回复" * 80)
            assert done_text == "长回复" * 80


def test_watch_ws_start_only_auto_finishes(tmp_path: Path):
    settings = Settings(
        data_dir=tmp_path,
        watch_token="watch-token",
        tts_provider="piper",
        ptt_max_s=1.0,
        empty_asr_prompt="用户只发送了开始、没有发送停止。请提示他再说一次。",
    )
    settings.ensure_dirs()
    fake_ai = FakeAI()
    app = create_app(settings, ai_service=fake_ai, tts_service=FakeTTS(), asr_service=NoASR())
    from fastapi.testclient import TestClient

    with TestClient(app) as client:
        with client.websocket_connect(
            "/v1/ws", headers={"Authorization": "Bearer watch-token"}
        ) as websocket:
            assert websocket.receive_json()["type"] == "hello"
            websocket.send_json({"type": "listen", "state": "start"})

            seen = []
            while True:
                message = websocket.receive()
                if message.get("type") == "websocket.close":
                    break
                if message.get("bytes") is not None:
                    continue
                payload = json.loads(message["text"])
                seen.append(payload["type"])
                if payload["type"] == "tts" and payload.get("state") == "stop":
                    break

            assert "llm" in seen
            assert "tts" in seen
            assert any("没有发送停止" in message["content"] for message in fake_ai.messages)


def test_watch_ws_no_audio_gets_ai_fallback(tmp_path: Path):
    settings = Settings(
        data_dir=tmp_path,
        watch_token="watch-token",
        tts_provider="piper",
        empty_asr_prompt="用户刚刚回复了空消息，请提示他再说一遍。",
    )
    settings.ensure_dirs()
    fake_ai = FakeAI()
    app = create_app(settings, ai_service=fake_ai, tts_service=FakeTTS(), asr_service=NoASR())
    from fastapi.testclient import TestClient

    with TestClient(app) as client:
        with client.websocket_connect(
            "/v1/ws", headers={"Authorization": "Bearer watch-token"}
        ) as websocket:
            hello = websocket.receive_json()
            assert hello["type"] == "hello"
            websocket.send_json({"type": "listen", "state": "start"})
            websocket.send_json({"type": "listen", "state": "stop"})

            seen = []
            llm_text = ""
            while True:
                message = websocket.receive()
                if message.get("type") == "websocket.close":
                    break
                if message.get("bytes") is not None:
                    continue
                payload = json.loads(message["text"])
                seen.append(payload["type"])
                if payload["type"] == "llm":
                    llm_text = payload.get("text", "")
                if payload["type"] == "tts" and payload.get("state") == "stop":
                    break

            assert "stt" in seen
            assert "llm" in seen
            assert "tts" in seen
            assert llm_text == "你好，我是测试助手。"
            assert any(
                "空消息" in message["content"] for message in fake_ai.messages
            )


def test_watch_ws_text_stt_llm_tts(tmp_path: Path):
    settings = Settings(
        data_dir=tmp_path,
        watch_token="watch-token",
        tts_provider="piper",
    )
    settings.ensure_dirs()
    app = create_app(settings, ai_service=FakeAI(), tts_service=FakeTTS(), asr_service=NoASR())
    from fastapi.testclient import TestClient

    with TestClient(app) as client:
        with client.websocket_connect(
            "/v1/ws", headers={"Authorization": "Bearer watch-token"}
        ) as websocket:
            websocket.send_json({
                "type": "hello",
                "version": 1,
                "transport": "websocket",
                "audio_params": {"format": "opus", "sample_rate": 16000, "channels": 1, "frame_duration": 60},
            })
            hello = websocket.receive_json()
            assert hello["type"] == "hello"
            websocket.send_json({"type": "listen", "state": "start"})
            websocket.send_json({"type": "listen", "state": "stop", "text": "你好"})
            seen = []
            binary = 0
            llm_payloads = []
            while True:
                message = websocket.receive()
                if message.get("type") == "websocket.close":
                    break
                if message.get("bytes") is not None:
                    binary += 1
                    continue
                payload = json.loads(message["text"])
                seen.append(payload["type"])
                if payload["type"] == "llm":
                    llm_payloads.append(payload)
                if payload["type"] == "tts" and payload.get("state") == "stop":
                    break
            assert "stt" in seen
            assert "llm" in seen
            assert "tts" in seen
            assert binary > 0
            assert llm_payloads[0]["state"] == "start"
            assert any(item["state"] == "delta" for item in llm_payloads)
            assert llm_payloads[-1]["state"] == "done"
            assert llm_payloads[-1]["text"] == "你好，我是测试助手。"
