"""Audio helpers: WAV/PCM conversion and raw Opus frames for the watch protocol."""

from __future__ import annotations

import io
import struct
import wave
from array import array
from collections.abc import Iterable


class AudioError(RuntimeError):
    pass


def _to_mono_i16(raw: bytes, sample_width: int, channels: int) -> array:
    if channels < 1:
        channels = 1
    if sample_width == 1:
        values = [(value - 128) << 8 for value in raw]
    elif sample_width == 2:
        values = list(struct.unpack("<%dh" % (len(raw) // 2), raw))
    elif sample_width == 3:
        values = []
        for offset in range(0, len(raw) - 2, 3):
            value = raw[offset] | (raw[offset + 1] << 8) | (raw[offset + 2] << 16)
            if value & 0x800000:
                value -= 0x1000000
            values.append(value >> 8)
    elif sample_width == 4:
        values = [value >> 16 for value in struct.unpack("<%di" % (len(raw) // 4), raw)]
    else:
        raise AudioError("unsupported WAV sample width: %d" % sample_width)

    if channels == 1:
        return array("h", values)

    mono = array("h")
    frames = len(values) // channels
    for frame in range(frames):
        base = frame * channels
        mono.append(int(sum(values[base:base + channels]) / channels))
    return mono


def _resample_mono(samples: array, source_rate: int, target_rate: int) -> array:
    if source_rate == target_rate or not samples:
        return samples
    ratio = source_rate / float(target_rate)
    out_len = max(1, int(len(samples) * target_rate / source_rate))
    result = array("h")
    last = len(samples) - 1
    for index in range(out_len):
        position = index * ratio
        left = int(position)
        if left >= last:
            result.append(samples[last])
            continue
        fraction = position - left
        value = samples[left] + (samples[left + 1] - samples[left]) * fraction
        result.append(max(-32768, min(32767, int(round(value)))))
    return result


def wav_to_pcm16(wav_bytes: bytes, target_rate: int = 24000) -> bytes:
    """Convert any RIFF/WAV to signed 16-bit mono PCM at target_rate."""
    if not wav_bytes:
        raise AudioError("empty WAV")
    try:
        with wave.open(io.BytesIO(wav_bytes), "rb") as wav:
            channels = wav.getnchannels()
            sample_width = wav.getsampwidth()
            rate = wav.getframerate()
            raw = wav.readframes(wav.getnframes())
    except (wave.Error, EOFError) as exc:
        raise AudioError("invalid WAV: %s" % exc) from exc
    if rate <= 0:
        raise AudioError("invalid WAV sample rate")
    samples = _to_mono_i16(raw, sample_width, channels)
    samples = _resample_mono(samples, rate, target_rate)
    return samples.tobytes()


def pcm16_to_opus_frames(
    pcm: bytes,
    sample_rate: int = 24000,
    frame_ms: int = 60,
) -> list[bytes]:
    """Encode PCM16 mono to raw Opus packets (the watch expects raw packets)."""
    if not pcm:
        return []
    try:
        import av  # type: ignore
        import numpy as np  # type: ignore
    except ImportError as exc:  # pragma: no cover - exercised in deployment
        raise AudioError("PyAV/numpy not installed; install requirements.txt") from exc

    from fractions import Fraction

    frame_samples = max(1, sample_rate * frame_ms // 1000)
    frame_bytes = frame_samples * 2
    context = av.CodecContext.create("libopus", "w")
    context.sample_rate = sample_rate
    context.format = "s16"
    context.layout = "mono"
    context.bit_rate = 24000
    # libopus defaults to 20 ms packets.  The watch decoder is configured for
    # 60 ms frames, so set FFmpeg's libopus option before the encoder opens.
    context.options = {"frame_duration": str(frame_ms)}
    packets: list[bytes] = []
    pts = 0
    for offset in range(0, len(pcm) - (len(pcm) % 2), frame_bytes):
        chunk = pcm[offset:offset + frame_bytes]
        if len(chunk) < 4:
            continue
        if len(chunk) % 2:
            chunk = chunk[:-1]
        samples = np.frombuffer(chunk, dtype=np.int16).reshape(1, -1)
        frame = av.AudioFrame.from_ndarray(samples, format="s16", layout="mono")
        frame.sample_rate = sample_rate
        frame.pts = pts
        frame.time_base = Fraction(1, sample_rate)
        pts += len(chunk) // 2
        for packet in context.encode(frame):
            if packet.size:
                packets.append(bytes(packet))
    try:
        for packet in context.encode(None):
            if packet.size:
                packets.append(bytes(packet))
    except Exception:
        pass
    return packets


def opus_packets_to_pcm16(
    packets: Iterable[bytes],
    *,
    target_rate: int = 16000,
    source_rate: int = 24000,
) -> bytes:
    """Decode raw Opus packets and resample to 16-bit mono PCM."""
    try:
        import av  # type: ignore
    except ImportError as exc:  # pragma: no cover
        raise AudioError("PyAV not installed; install requirements.txt") from exc

    decoder = av.CodecContext.create("libopus", "r")
    resampler = av.AudioResampler(format="s16", layout="mono", rate=target_rate)
    output = bytearray()
    for packet in packets:
        if not packet:
            continue
        try:
            frames = decoder.decode(av.Packet(packet))
        except Exception as exc:  # malformed packet should not kill the session
            raise AudioError("Opus decode failed: %s" % exc) from exc
        for frame in frames:
            converted = resampler.resample(frame)
            if converted is None:
                continue
            if not isinstance(converted, (list, tuple)):
                converted = [converted]
            for audio_frame in converted:
                output.extend(audio_frame.to_ndarray().tobytes())
    return bytes(output)


def pcm16_to_wav(pcm: bytes, sample_rate: int = 16000) -> bytes:
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm)
    return output.getvalue()
