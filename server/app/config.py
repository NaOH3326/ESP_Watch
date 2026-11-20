"""Environment-backed configuration for the watch server."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

try:  # python-dotenv is optional at import time for tests
    from dotenv import load_dotenv

    load_dotenv()
except Exception:  # pragma: no cover - dotenv is a convenience only
    pass


def _env_str(name: str, default: str = "") -> str:
    value = os.getenv(name)
    return default if value is None else value.strip()


def _env_bool(name: str, default: bool) -> bool:
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.getenv(name, str(default)))
    except (TypeError, ValueError):
        return default


def _env_float(name: str, default: float) -> float:
    try:
        return float(os.getenv(name, str(default)))
    except (TypeError, ValueError):
        return default


DEFAULT_SYSTEM_PROMPT = (
    "你是一个运行在自建服务器上的中文 AI 助手。回答准确、简洁，"
    "需要时给出可执行的步骤。不要编造事实。"
)
DEFAULT_WATCH_SYSTEM_PROMPT = (
    "你是 ESP32 手表上的中文语音助手。回答长度由用户的请求决定："
    "用户没有指定长度时保持简洁；用户明确要求详细说明或指定字数时，"
    "按用户要求完整回答，不要自行截断。"
    "除非用户明确要求，否则不要使用 Markdown、表格或长代码。"
)


@dataclass
class Settings:
    """All runtime options.  Paths are resolved in __post_init__."""

    data_dir: Path = field(default_factory=lambda: Path(_env_str("DATA_DIR", str(Path.cwd() / "data"))))
    db_path: Path | None = None
    files_dir: Path | None = None
    tts_cache_dir: Path | None = None

    max_upload_mb: int = field(default_factory=lambda: _env_int("MAX_UPLOAD_MB", 512))
    admin_token: str = field(default_factory=lambda: _env_str("ADMIN_TOKEN"))
    watch_token: str = field(default_factory=lambda: _env_str("WATCH_TOKEN"))

    deepseek_api_key: str = field(default_factory=lambda: _env_str("DEEPSEEK_API_KEY"))
    deepseek_base_url: str = field(default_factory=lambda: _env_str("DEEPSEEK_BASE_URL", "https://api.deepseek.com").rstrip("/"))
    deepseek_model: str = field(default_factory=lambda: _env_str("DEEPSEEK_MODEL", "deepseek-chat"))
    ai_temperature: float = field(default_factory=lambda: _env_float("AI_TEMPERATURE", 0.7))
    ai_timeout_s: float = field(default_factory=lambda: _env_float("AI_TIMEOUT_S", 120.0))
    ai_max_tokens: int = field(default_factory=lambda: _env_int("AI_MAX_TOKENS", 1024))
    system_prompt: str = field(default_factory=lambda: _env_str("AI_SYSTEM_PROMPT", DEFAULT_SYSTEM_PROMPT))
    watch_system_prompt: str = field(default_factory=lambda: _env_str("WATCH_SYSTEM_PROMPT", DEFAULT_WATCH_SYSTEM_PROMPT))

    tts_provider: str = field(default_factory=lambda: _env_str("TTS_PROVIDER", "piper").lower())
    tts_cache: bool = field(default_factory=lambda: _env_bool("TTS_CACHE", True))
    piper_voice: str = field(default_factory=lambda: _env_str("PIPER_VOICE", "zh_CN-huayan-medium"))
    piper_model: str = field(default_factory=lambda: _env_str("PIPER_MODEL"))
    piper_model_dir: Path | None = None
    piper_url: str = field(default_factory=lambda: _env_str("PIPER_URL").rstrip("/"))
    piper_bin: str = field(default_factory=lambda: _env_str("PIPER_BIN", "piper"))
    cosyvoice_url: str = field(default_factory=lambda: _env_str("COSYVOICE_URL"))
    cosyvoice_token: str = field(default_factory=lambda: _env_str("COSYVOICE_TOKEN"))
    cosyvoice_speaker: str = field(default_factory=lambda: _env_str("COSYVOICE_SPEAKER", "中文女"))
    tts_timeout_s: float = field(default_factory=lambda: _env_float("TTS_TIMEOUT_S", 120.0))

    asr_provider: str = field(default_factory=lambda: _env_str("ASR_PROVIDER", "none").lower())
    asr_base_url: str = field(default_factory=lambda: _env_str("ASR_BASE_URL", "https://api.openai.com/v1").rstrip("/"))
    asr_api_key: str = field(default_factory=lambda: _env_str("ASR_API_KEY"))
    asr_model: str = field(default_factory=lambda: _env_str("ASR_MODEL", "whisper-1"))
    asr_language: str = field(default_factory=lambda: _env_str("ASR_LANGUAGE", "zh"))
    asr_prompt: str = field(default_factory=lambda: _env_str("ASR_PROMPT"))
    asr_timeout_s: float = field(default_factory=lambda: _env_float("ASR_TIMEOUT_S", 60.0))
    empty_asr_prompt: str = field(
        default_factory=lambda: _env_str(
            "EMPTY_ASR_PROMPT",
            "用户刚刚按下了语音键，但没有检测到语音内容（空音频）。"
            "请用简短中文回复，提示用户再说一遍。",
        )
    )
    ptt_max_s: float = field(default_factory=lambda: _env_float("PTT_MAX_S", 30.0))

    watch_video_width: int = field(default_factory=lambda: _env_int("WATCH_VIDEO_WIDTH", 204))
    watch_video_height: int = field(default_factory=lambda: _env_int("WATCH_VIDEO_HEIGHT", 138))
    watch_video_fps: int = field(default_factory=lambda: _env_int("WATCH_VIDEO_FPS", 12))
    watch_video_quality: int = field(default_factory=lambda: _env_int("WATCH_VIDEO_QUALITY", 72))
    watch_video_audio: bool = field(default_factory=lambda: _env_bool("WATCH_VIDEO_AUDIO", True))
    watch_audio_ms: int = field(default_factory=lambda: _env_int("WATCH_AUDIO_MS", 20))
    watch_ai_sample_rate: int = field(default_factory=lambda: _env_int("WATCH_AI_SAMPLE_RATE", 24000))
    watch_ai_frame_ms: int = field(default_factory=lambda: _env_int("WATCH_AI_FRAME_MS", 60))

    log_level: str = field(default_factory=lambda: _env_str("LOG_LEVEL", "INFO").upper())


    def __post_init__(self) -> None:
        self.data_dir = Path(self.data_dir).expanduser().resolve()
        self.db_path = Path(self.db_path).expanduser().resolve() if self.db_path else self.data_dir / "watch.db"
        self.files_dir = Path(self.files_dir).expanduser().resolve() if self.files_dir else self.data_dir / "files"
        self.tts_cache_dir = (
            Path(self.tts_cache_dir).expanduser().resolve()
            if self.tts_cache_dir
            else self.data_dir / "tts-cache"
        )
        self.piper_model_dir = (
            Path(self.piper_model_dir).expanduser().resolve()
            if self.piper_model_dir
            else self.data_dir / "models" / "piper"
        )
        if self.max_upload_mb < 1:
            self.max_upload_mb = 1
        if self.watch_video_width < 16:
            self.watch_video_width = 204
        if self.watch_video_height < 16:
            self.watch_video_height = 138
        if self.watch_video_fps < 1:
            self.watch_video_fps = 12
        if not 1 <= self.watch_video_quality <= 95:
            self.watch_video_quality = 72
        if self.watch_ai_sample_rate < 8000:
            self.watch_ai_sample_rate = 24000
        if self.watch_ai_frame_ms not in (20, 40, 60):
            self.watch_ai_frame_ms = 60

    @classmethod
    def from_env(cls) -> "Settings":
        return cls()

    def ensure_dirs(self) -> None:
        for path in (self.data_dir, self.files_dir, self.tts_cache_dir, self.piper_model_dir):
            path.mkdir(parents=True, exist_ok=True)
        self.db_path.parent.mkdir(parents=True, exist_ok=True)

    @property
    def max_upload_bytes(self) -> int:
        return self.max_upload_mb * 1024 * 1024

    def public_summary(self) -> dict:
        """Safe configuration snapshot for /api/status."""
        return {
            "deepseek_model": self.deepseek_model,
            "deepseek_configured": bool(self.deepseek_api_key),
            "tts_provider": self.tts_provider,
            "tts_voice": (
                self.piper_voice
                if self.tts_provider == "piper"
                else self.cosyvoice_speaker if self.tts_provider == "cosyvoice" else ""
            ),
            "asr_provider": self.asr_provider,
            "asr_model": self.asr_model,
            "max_upload_mb": self.max_upload_mb,
            "auth_required": bool(self.admin_token),
            "watch_auth_required": bool(self.watch_token),
        }
