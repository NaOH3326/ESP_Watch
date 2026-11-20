"""DeepSeek chat backend for the watch server."""

from __future__ import annotations

import json
from collections.abc import AsyncIterator
from typing import Any

import httpx

from .config import Settings


class AIError(RuntimeError):
    def __init__(self, message: str, status_code: int = 502) -> None:
        super().__init__(message)
        self.status_code = status_code


def _clean_messages(messages: list[dict[str, Any]]) -> list[dict[str, str]]:
    cleaned: list[dict[str, str]] = []
    for item in messages or []:
        if not isinstance(item, dict):
            continue
        role = str(item.get("role", "")).lower()
        content = item.get("content", "")
        if role not in {"system", "user", "assistant"}:
            continue
        if not isinstance(content, str):
            content = str(content)
        content = content.strip()
        if content:
            cleaned.append({"role": role, "content": content[:12000]})
    if not any(item["role"] == "user" for item in cleaned):
        raise AIError("at least one user message is required", 400)
    return cleaned[-30:]


def _deepseek_url(base: str) -> str:
    base = base.rstrip("/")
    if base.endswith("/chat/completions"):
        return base
    return base + "/chat/completions"


class AIService:
    """Small OpenAI-compatible client dedicated to the DeepSeek chat API."""

    def __init__(self, settings: Settings) -> None:
        self.settings = settings

    def resolve_provider(self, provider: str | None = None) -> str:
        selected = (provider or "deepseek").strip().lower()
        if selected in {"", "auto", "deepseek"}:
            return "deepseek"
        raise AIError("only the DeepSeek chat provider is supported", 400)

    def default_model(self, provider: str = "deepseek") -> str:
        del provider
        return self.settings.deepseek_model

    async def provider_status(self) -> dict[str, Any]:
        configured = bool(self.settings.deepseek_api_key)
        return {
            "default": "deepseek",
            "providers": {
                "deepseek": {
                    "available": configured,
                    "model": self.settings.deepseek_model,
                    "detail": "API key configured" if configured else "DEEPSEEK_API_KEY is empty",
                },
            },
        }

    async def stream(
        self,
        messages: list[dict[str, Any]],
        *,
        provider: str | None = None,
        model: str | None = None,
        temperature: float | None = None,
        max_tokens: int | None = None,
    ) -> AsyncIterator[str]:
        selected = self.resolve_provider(provider)
        chosen_model = (model or self.default_model(selected)).strip()
        if not chosen_model:
            raise AIError("model is empty", 400)
        clean = _clean_messages(messages)
        temp = self.settings.ai_temperature if temperature is None else float(temperature)
        limit = self.settings.ai_max_tokens if max_tokens is None else int(max_tokens)
        async for chunk in self._stream_deepseek(clean, chosen_model, temp, limit):
            yield chunk

    async def _stream_deepseek(
        self,
        messages: list[dict[str, str]],
        model: str,
        temperature: float,
        max_tokens: int,
    ) -> AsyncIterator[str]:
        if not self.settings.deepseek_api_key:
            raise AIError("DEEPSEEK_API_KEY is not configured", 503)
        payload = {
            "model": model,
            "messages": messages,
            "stream": True,
            "temperature": temperature,
            "max_tokens": max_tokens,
        }
        headers = {
            "Authorization": "Bearer " + self.settings.deepseek_api_key,
            "Content-Type": "application/json",
            "Accept": "text/event-stream",
        }
        timeout = httpx.Timeout(self.settings.ai_timeout_s, connect=8.0)
        url = _deepseek_url(self.settings.deepseek_base_url)
        try:
            async with httpx.AsyncClient(timeout=timeout) as client:
                async with client.stream("POST", url, headers=headers, json=payload) as response:
                    if response.status_code != 200:
                        body = (await response.aread()).decode("utf-8", "replace")
                        raise AIError("DeepSeek HTTP %d: %s" % (response.status_code, body[:300]))
                    async for line in response.aiter_lines():
                        line = line.strip()
                        if not line or line.startswith(":"):
                            continue
                        if not line.startswith("data:"):
                            continue
                        data = line[5:].strip()
                        if data == "[DONE]":
                            return
                        try:
                            event = json.loads(data)
                        except json.JSONDecodeError:
                            continue
                        if event.get("error"):
                            raise AIError(str(event["error"])[:300])
                        choices = event.get("choices") or []
                        if not choices:
                            continue
                        delta = choices[0].get("delta") or {}
                        content = delta.get("content") or ""
                        if content:
                            yield content
        except AIError:
            raise
        except httpx.TimeoutException as exc:
            raise AIError("DeepSeek timeout: %s" % exc) from exc
        except httpx.HTTPError as exc:
            raise AIError("DeepSeek request failed: %s" % exc) from exc
