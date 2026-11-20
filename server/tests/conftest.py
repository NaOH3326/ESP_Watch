from __future__ import annotations

from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app


def make_settings(tmp_path: Path, **overrides) -> Settings:
    values = {
        "data_dir": tmp_path,
        "admin_token": "",
        "watch_token": "watch-token",
        "tts_provider": "none",
        "asr_provider": "none",
        "watch_video_width": 220,
        "watch_video_height": 138,
    }
    values.update(overrides)
    settings = Settings(**values)
    settings.ensure_dirs()
    return settings


@pytest.fixture()
def client(tmp_path: Path):
    app = create_app(make_settings(tmp_path))
    with TestClient(app) as test_client:
        yield test_client


@pytest.fixture()
def protected_client(tmp_path: Path):
    app = create_app(make_settings(tmp_path, admin_token="secret"))
    with TestClient(app) as test_client:
        yield test_client
