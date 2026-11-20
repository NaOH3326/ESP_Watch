#!/usr/bin/env python3
"""Send an audio/video stream to WatchShell's WSV2 receiver.

Requirements:
    pip install av pillow

The watch listens on TCP port 3333 while the MEDIA -> VIDEO tab is open.
Video frames are JPEGs scaled by this sender.  Audio is signed 16-bit
little-endian mono PCM at 16 kHz.  The receiver only decodes/plays; it does
not resize or resample the source.
"""

import argparse
import io
import socket
import struct
import sys
import time

try:
    import av
    from PIL import Image
except ImportError:
    print("install dependencies: pip install av pillow", file=sys.stderr)
    raise

try:
    RESAMPLE = Image.Resampling.LANCZOS
except AttributeError:  # Pillow < 9
    RESAMPLE = Image.LANCZOS

MAGIC = b"WSV2"
HEADER = struct.Struct("<4sBBHHHII")
TYPE_JPEG = 1
TYPE_PCM16 = 2


def send_message(sock, msg_type, width, height, timestamp, payload):
    header = HEADER.pack(MAGIC, msg_type, 0, width, height, 0,
                         int(timestamp * 1000) & 0xFFFFFFFF, len(payload))
    sock.sendall(header)
    sock.sendall(payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="video file or capture device")
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--width", type=int, default=220)
    parser.add_argument("--height", type=int, default=138)
    parser.add_argument("--quality", type=int, default=72)
    parser.add_argument("--audio-ms", type=int, default=20)
    args = parser.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    container = av.open(args.source)
    video = next((s for s in container.streams if s.type == "video"), None)
    audio = next((s for s in container.streams if s.type == "audio"), None)
    if video is None:
        print("source has no video stream", file=sys.stderr)
        return 1
    video.thread_type = "AUTO"
    streams = [video] + ([audio] if audio else [])

    resampler = None
    if audio:
        audio.thread_type = "AUTO"
        resampler = av.AudioResampler(format="s16", layout="mono", rate=16000)

    media_start = None
    wall_start = None

    def pace(timestamp):
        nonlocal media_start, wall_start
        if timestamp is None:
            return
        if media_start is None:
            media_start = timestamp
            wall_start = time.monotonic()
        delay = timestamp - media_start - (time.monotonic() - wall_start)
        if delay > 0:
            time.sleep(min(delay, 0.5))

    audio_bytes = max(2, args.audio_ms * 16000 // 1000 * 2)

    try:
        for packet in container.demux(*streams):
            for frame in packet.decode():
                if isinstance(frame, av.VideoFrame):
                    timestamp = float(frame.pts * video.time_base) if frame.pts is not None else None
                    pace(timestamp)
                    image = frame.to_image()
                    image.thumbnail((args.width, args.height), RESAMPLE)
                    buf = io.BytesIO()
                    image.save(buf, format="JPEG", quality=args.quality)
                    send_message(sock, TYPE_JPEG, image.width, image.height,
                                 timestamp or 0.0, buf.getvalue())
                elif isinstance(frame, av.AudioFrame) and resampler:
                    timestamp = float(frame.pts * audio.time_base) if frame.pts is not None else None
                    pace(timestamp)
                    frames = resampler.resample(frame)
                    if frames is None:
                        continue
                    if not isinstance(frames, (list, tuple)):
                        frames = [frames]
                    for af in frames:
                        pcm = af.to_ndarray().tobytes()
                        for off in range(0, len(pcm), audio_bytes):
                            chunk = pcm[off:off + audio_bytes]
                            if len(chunk) & 1:
                                chunk = chunk[:-1]
                            if chunk:
                                send_message(sock, TYPE_PCM16, 0, 0,
                                             timestamp or 0.0, chunk)
    finally:
        sock.close()
        container.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
