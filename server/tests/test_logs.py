from __future__ import annotations

import logging
from pathlib import Path

from app.config import Settings
from app.main import create_app


def test_logs_endpoint_requires_admin(tmp_path: Path):
    settings = Settings(data_dir=tmp_path, admin_token="admin", tts_provider="none")
    settings.ensure_dirs()
    app = create_app(settings)
    from fastapi.testclient import TestClient

    with TestClient(app) as client:
        assert client.get("/api/logs").status_code == 401


def test_logs_endpoint_returns_cursor_and_messages(tmp_path: Path):
    settings = Settings(data_dir=tmp_path, admin_token="admin", tts_provider="none")
    settings.ensure_dirs()
    app = create_app(settings)
    logging.getLogger("watch-server").info("dashboard log probe")
    from fastapi.testclient import TestClient

    headers = {"Authorization": "Bearer admin"}
    with TestClient(app) as client:
        response = client.get("/api/logs?limit=100", headers=headers)
        assert response.status_code == 200
        data = response.json()
        assert data["cursor"] >= 1
        assert any(item["message"] == "dashboard log probe" for item in data["items"])
        cursor = data["cursor"]

        empty = client.get("/api/logs?after=" + str(cursor), headers=headers).json()
        assert all(item["id"] > cursor for item in empty["items"])
