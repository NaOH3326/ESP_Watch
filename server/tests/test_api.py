from __future__ import annotations

from pathlib import Path

import pytest
from starlette.websockets import WebSocketDisconnect

FAKE_VIDEO = (b"\x00\x00\x00\x18ftypmp42" + bytes(range(250)) * 4)[:1024]


def test_health_and_status(client):
    assert client.get("/healthz").json()["status"] == "ok"
    status = client.get("/api/status")
    assert status.status_code == 200
    data = status.json()
    assert data["status"] == "ok"
    assert data["config"]["auth_required"] is False
    assert data["config"]["watch_auth_required"] is True


def test_upload_list_range_and_delete(client):
    response = client.post(
        "/api/files",
        files={"file": ("clip.mp4", FAKE_VIDEO, "video/mp4")},
    )
    assert response.status_code == 201, response.text
    item = response.json()
    assert item["kind"] == "video"
    assert item["size"] == len(FAKE_VIDEO)

    listed = client.get("/api/files").json()
    assert listed["items"][0]["id"] == item["id"]

    ranged = client.get(
        "/api/files/%s/content" % item["id"],
        headers={"Range": "bytes=0-9"},
    )
    assert ranged.status_code == 206
    assert ranged.headers["content-range"] == "bytes 0-9/%d" % len(FAKE_VIDEO)
    assert ranged.content == FAKE_VIDEO[:10]

    head = client.head("/api/files/%s/content" % item["id"])
    assert head.status_code == 200
    assert head.headers["accept-ranges"] == "bytes"

    catalog = client.get(
        "/watch/api/v1/videos",
        headers={"Authorization": "Bearer watch-token"},
    )
    assert catalog.status_code == 200
    payload = catalog.json()
    assert payload["version"] == 1
    assert payload["items"][0]["id"] == item["id"]
    assert payload["items"][0]["title"] == "clip"

    deleted = client.delete("/api/files/%s" % item["id"])
    assert deleted.status_code == 200
    assert client.get("/api/files").json()["items"] == []


def test_range_rejects_bad_request(client):
    response = client.post(
        "/api/files",
        files={"file": ("tiny.bin", b"0123456789", "application/octet-stream")},
    )
    item = response.json()
    bad = client.get(
        "/api/files/%s/content" % item["id"],
        headers={"Range": "bytes=100-200"},
    )
    assert bad.status_code == 416


def test_admin_token_protects_api(protected_client):
    assert protected_client.get("/api/files").status_code == 401
    ok = protected_client.get("/api/files", headers={"Authorization": "Bearer secret"})
    assert ok.status_code == 200
    query = protected_client.get("/api/files?token=secret")
    assert query.status_code == 200


def test_watch_catalog_requires_token(client):
    assert client.get("/watch/api/v1/videos").status_code == 401
    assert client.get(
        "/watch/api/v1/videos",
        headers={"Authorization": "Bearer wrong"},
    ).status_code == 401


def test_chat_validates_messages(client):
    response = client.post("/api/chat", json={"messages": [], "stream": False})
    assert response.status_code in (400, 502)


def test_tts_disabled_returns_503(client):
    response = client.post("/api/tts", json={"text": "hello"})
    assert response.status_code == 503
    assert "TTS" in response.text


def test_rejects_invalid_kind(client):
    response = client.get("/api/files?kind=does-not-exist")
    assert response.status_code == 400


def test_watch_websocket_hello(client):
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
        assert hello["transport"] == "websocket"
        assert hello["audio_params"]["sample_rate"] == 24000


def test_ai_providers_is_deepseek_only(client):
    data = client.get("/api/ai/providers").json()
    assert data["default"] == "deepseek"
    assert set(data["providers"]) == {"deepseek"}


def test_asr_status_shape(client):
    data = client.get("/api/status").json()
    asr = data["asr"]
    assert asr["provider"] == "none"
    assert asr["available"] is False
    assert "ASR" in asr["detail"]


def test_asr_status_configured(tmp_path: Path):
    from app.config import Settings
    from app.main import create_app

    settings = Settings(
        data_dir=tmp_path,
        watch_token="watch-token",
        tts_provider="none",
        asr_provider="openai_compatible",
        asr_api_key="asr-key",
        asr_base_url="https://api.example.test/v1",
        asr_model="whisper-1",
    )
    settings.ensure_dirs()
    app = create_app(settings)
    from fastapi.testclient import TestClient

    with TestClient(app) as client:
        asr = client.get("/api/status").json()["asr"]
        assert asr["available"] is True
        assert asr["provider"] == "openai_compatible"
        assert asr["language"] == "zh"


def test_asr_status_accepts_local_endpoint_without_key(tmp_path: Path):
    from app.config import Settings
    from app.main import create_app

    settings = Settings(
        data_dir=tmp_path,
        watch_token="watch-token",
        tts_provider="none",
        asr_provider="openai_compatible",
        asr_api_key="",
        asr_base_url="http://speaches:8000/v1",
        asr_model="Systran/faster-whisper-small",
    )
    settings.ensure_dirs()
    app = create_app(settings)
    from fastapi.testclient import TestClient

    with TestClient(app) as client:
        asr = client.get("/api/status").json()["asr"]
        assert asr["available"] is True
        assert "no API key" in asr["detail"]


def test_watch_websocket_ping_pong(client):
    with client.websocket_connect(
        "/v1/ws", headers={"Authorization": "Bearer watch-token"}
    ) as websocket:
        hello = websocket.receive_json()
        assert hello["type"] == "hello"
        websocket.send_json({"type": "ping"})
        pong = websocket.receive_json()
        assert pong["type"] == "pong"
        assert pong["session_id"] == hello["session_id"]


def test_watch_websocket_rejects_bad_token(client):
    with pytest.raises(WebSocketDisconnect):
        with client.websocket_connect(
            "/v1/ws", headers={"Authorization": "Bearer wrong"}
        ) as websocket:
            websocket.receive_json()
