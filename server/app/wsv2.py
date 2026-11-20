"""WSV2 HTTP pull stream: baseline JPEG frames + 16 kHz mono PCM16."""

from __future__ import annotations

import io
import struct
import time
from collections.abc import Iterator
from pathlib import Path

WSV2_HEADER = struct.Struct("<4sBBHHHII")
WSV2_MAGIC = b"WSV2"
TYPE_JPEG = 1
TYPE_PCM16 = 2


class WSV2Error(RuntimeError):
    pass


def _load_deps():
    try:
        import av  # type: ignore
        from PIL import Image  # type: ignore
    except ImportError as exc:  # pragma: no cover - deployment dependency
        raise WSV2Error("PyAV/Pillow not installed; install requirements.txt") from exc
    return av, Image


def probe_video(path: Path) -> dict[str, int]:
    """Return width, height and duration in milliseconds; zeros if unknown."""
    try:
        av, _ = _load_deps()
        with av.open(str(path)) as container:
            video = next((s for s in container.streams if s.type == "video"), None)
            if video is None:
                return {"width": 0, "height": 0, "duration_ms": 0}
            duration = 0
            if container.duration:
                duration = int(float(container.duration) / 1_000_000 * 1000)
            return {
                "width": int(video.codec_context.width or 0),
                "height": int(video.codec_context.height or 0),
                "duration_ms": max(0, duration),
            }
    except Exception:
        return {"width": 0, "height": 0, "duration_ms": 0}


def _wsv2(msg_type: int, width: int, height: int, timestamp: float, payload: bytes) -> bytes:
    header = WSV2_HEADER.pack(
        WSV2_MAGIC,
        msg_type,
        0,
        max(0, min(65535, width)),
        max(0, min(65535, height)),
        0,
        int(timestamp * 1000) & 0xFFFFFFFF,
        len(payload),
    )
    return header + payload


def stream_video(
    path: Path,
    *,
    width: int = 220,
    height: int = 138,
    quality: int = 72,
    audio_ms: int = 20,
    fps: int = 12,
    enable_audio: bool = True,
) -> Iterator[bytes]:
    """Yield WSV2 frames in real time.

    The watch has no MP4/H.264 decoder, so this function transcodes on demand
    to baseline JPEG and PCM16.  It intentionally never buffers the whole file.
    """
    av, Image = _load_deps()
    resample_filter = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
    container = av.open(str(path))
    video = next((s for s in container.streams if s.type == "video"), None)
    audio = next((s for s in container.streams if s.type == "audio"), None) if enable_audio else None
    if video is None:
        container.close()
        raise WSV2Error("source has no video stream")
    streams = [video] + ([audio] if audio is not None else [])
    resampler = None
    if audio is not None:
        resampler = av.AudioResampler(format="s16", layout="mono", rate=16000)

    audio_bytes = max(2, audio_ms * 16000 // 1000 * 2)
    audio_buffer = bytearray()
    media_start: float | None = None
    wall_start = 0.0
    last_video_ts: float | None = None
    # A tiny tolerance prevents 12 fps sources from losing every other frame
    # due to floating-point timestamps such as 0.083333333333.
    min_video_interval = max(0.0, 1.0 / max(1, fps) - 0.001)

    def pace(timestamp: float | None) -> None:
        nonlocal media_start, wall_start
        if timestamp is None:
            return
        if media_start is None:
            media_start = timestamp
            wall_start = time.monotonic()
        delay = timestamp - media_start - (time.monotonic() - wall_start)
        if delay > 0:
            time.sleep(min(delay, 0.5))

    try:
        for packet in container.demux(*streams):
            for frame in packet.decode():
                if isinstance(frame, av.VideoFrame):
                    timestamp = float(frame.pts * video.time_base) if frame.pts is not None else None
                    if timestamp is not None and last_video_ts is not None:
                        if timestamp - last_video_ts < min_video_interval:
                            continue
                    pace(timestamp)
                    if timestamp is not None:
                        last_video_ts = timestamp
                    image = frame.to_image()
                    image.thumbnail((width, height), resample_filter)
                    if image.mode != "RGB":
                        image = image.convert("RGB")
                    buffer = io.BytesIO()
                    image.save(buffer, format="JPEG", quality=quality,
                               optimize=False, progressive=False)
                    yield _wsv2(TYPE_JPEG, image.width, image.height,
                                timestamp or 0.0, buffer.getvalue())
                elif isinstance(frame, av.AudioFrame) and resampler is not None:
                    timestamp = float(frame.pts * audio.time_base) if frame.pts is not None else None
                    pace(timestamp)
                    converted = resampler.resample(frame)
                    if converted is None:
                        continue
                    if not isinstance(converted, (list, tuple)):
                        converted = [converted]
                    for audio_frame in converted:
                        audio_buffer.extend(audio_frame.to_ndarray().tobytes())
                    while len(audio_buffer) >= audio_bytes:
                        chunk = bytes(audio_buffer[:audio_bytes])
                        del audio_buffer[:audio_bytes]
                        yield _wsv2(TYPE_PCM16, 0, 0, timestamp or 0.0, chunk)
        if audio_buffer:
            chunk = bytes(audio_buffer[: len(audio_buffer) & ~1])
            if chunk:
                yield _wsv2(TYPE_PCM16, 0, 0, 0.0, chunk)
    finally:
        container.close()
