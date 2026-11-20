from __future__ import annotations

from pathlib import Path

from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app


def _client(tmp_path: Path, **overrides) -> TestClient:
    values = {
        "data_dir": tmp_path,
        "admin_token": "",
        "watch_token": "watch-token",
        "tts_provider": "piper",
        "asr_provider": "none",
    }
    values.update(overrides)
    settings = Settings(**values)
    settings.ensure_dirs()
    return TestClient(create_app(settings))


def _upload(client: TestClient, name: str = "demo", *, config: bool = True, data: bytes = b"ONNX"):
    files = {
        "file": (name + ".onnx", data, "application/octet-stream"),
    }
    if config:
        files["config"] = (
            name + ".onnx.json",
            b'{"audio": {"sample_rate": 22050}, "phoneme_id_map": {}}',
            "application/json",
        )
    return client.post(
        "/api/tts/voices",
        data={"name": name, "activate": "true"},
        files=files,
    )


def test_upload_list_select_and_delete_tts_voice(tmp_path: Path):
    with _client(tmp_path) as client:
        response = _upload(client)
        assert response.status_code == 201, response.text
        voice = response.json()["voice"]
        assert voice["id"] == "demo"
        assert voice["active"] is True
        assert voice["has_config"] is True
        assert (tmp_path / "models" / "piper" / "demo.onnx").read_bytes() == b"ONNX"
        assert (tmp_path / "models" / "piper" / "demo.onnx.json").is_file()

        listing = client.get("/api/tts/voices").json()
        assert listing["active"] == "demo"
        assert [item["id"] for item in listing["voices"]] == ["demo"]
        assert listing["voices"][0]["size"] == 4

        # Upload a second voice and explicitly select it for the watch.
        second = _upload(client, "second")
        assert second.status_code == 201
        selected = client.post("/api/tts/voices/second/select")
        assert selected.status_code == 200
        assert selected.json()["voice"]["active"] is True
        assert client.get("/api/tts/voices").json()["active"] == "second"

        deleted = client.delete("/api/tts/voices/second")
        assert deleted.status_code == 200
        assert deleted.json()["deleted"] == "second"
        assert [item["id"] for item in client.get("/api/tts/voices").json()["voices"]] == ["demo"]


def test_tts_voice_requires_config_for_new_model(tmp_path: Path):
    with _client(tmp_path) as client:
        response = _upload(client, "no-config", config=False)
        assert response.status_code == 400
        assert "onnx.json" in response.text
        assert not (tmp_path / "models" / "piper" / "no-config.onnx").exists()


def test_tts_voice_rejects_invalid_config_and_duplicate(tmp_path: Path):
    with _client(tmp_path) as client:
        assert _upload(client).status_code == 201
        duplicate = _upload(client)
        assert duplicate.status_code == 409

        bad = client.post(
            "/api/tts/voices",
            data={"name": "bad"},
            files={
                "file": ("bad.onnx", b"ONNX", "application/octet-stream"),
                "config": ("bad.onnx.json", b"not-json", "application/json"),
            },
        )
        assert bad.status_code == 400
        assert not (tmp_path / "models" / "piper" / "bad.onnx").exists()


def test_tts_voice_name_is_path_safe(tmp_path: Path):
    with _client(tmp_path) as client:
        response = _upload(client, "../escaped")
        assert response.status_code == 201
        names = [item["id"] for item in client.get("/api/tts/voices").json()["voices"]]
        assert "escaped" in names
        assert not (tmp_path.parent / "escaped.onnx").exists()
