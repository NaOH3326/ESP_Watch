"""Bearer/query token authentication for admin and watch APIs."""

from __future__ import annotations

import secrets
from collections.abc import Callable

from fastapi import Header, HTTPException, Query, Request, WebSocket

from .config import Settings


def _bearer(value: str | None) -> str:
    if not value:
        return ""
    value = value.strip()
    if value.lower().startswith("bearer "):
        return value[7:].strip()
    return value


def _match(expected: str, supplied: str) -> bool:
    if not expected:
        return True
    return bool(supplied) and secrets.compare_digest(expected, supplied)


def extract_admin_token(
    request: Request,
    authorization: str | None = Header(default=None),
    x_admin_token: str | None = Header(default=None),
    token: str | None = Query(default=None),
) -> str:
    return _bearer(authorization) or (x_admin_token or "") or (token or "")


def admin_guard(settings: Settings) -> Callable[..., None]:
    def guard(
        request: Request,
        authorization: str | None = Header(default=None),
        x_admin_token: str | None = Header(default=None),
        token: str | None = Query(default=None),
    ) -> None:
        supplied = _bearer(authorization) or (x_admin_token or "") or (token or "")
        if not _match(settings.admin_token, supplied):
            raise HTTPException(
                status_code=401,
                detail="admin token required",
                headers={"WWW-Authenticate": "Bearer"},
            )

    return guard


def watch_authorized(settings: Settings, headers: dict[str, str], query_token: str = "") -> bool:
    supplied = _bearer(headers.get("authorization")) or headers.get("x-watch-token", "") or query_token
    return _match(settings.watch_token, supplied)


def check_watch_http(settings: Settings, request: Request) -> None:
    headers = {k.lower(): v for k, v in request.headers.items()}
    if not watch_authorized(settings, headers, request.query_params.get("token", "")):
        raise HTTPException(
            status_code=401,
            detail="watch token required",
            headers={"WWW-Authenticate": "Bearer"},
        )


async def check_watch_ws(websocket: WebSocket, settings: Settings) -> bool:
    headers = {k.lower(): v for k, v in websocket.headers.items()}
    token = websocket.query_params.get("token", "")
    if watch_authorized(settings, headers, token):
        return True
    await websocket.close(code=4401, reason="watch token required")
    return False
