from __future__ import annotations

import math
import struct

import pytest

pytest.importorskip("av")
pytest.importorskip("numpy")

from app.audio import opus_packets_to_pcm16, pcm16_to_opus_frames  # noqa: E402


def test_opus_round_trip_is_60ms_framed():
    rate = 24000
    samples = rate * 3 // 10  # 300 ms
    pcm = b"".join(
        struct.pack("<h", int(9000 * math.sin(2 * math.pi * 440 * i / rate)))
        for i in range(samples)
    )
    packets = pcm16_to_opus_frames(pcm, rate, 60)
    assert 5 <= len(packets) <= 6  # 5 x 60 ms plus encoder flush
    decoded = opus_packets_to_pcm16(packets, target_rate=16000)
    assert len(decoded) > 5000
