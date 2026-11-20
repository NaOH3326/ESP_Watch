#!/usr/bin/env python3
"""Reference catalog + WSV2 HTTP pull server for WatchShell.

Usage:
    pip install av pillow
    python3 tools/watch_media_server.py --media-dir ./videos --port 8080

Set VIDEO CATALOG URL in the watch portal to:
    http://<this-host>:8080/watch/api/v1/videos

The server lists video files, transcodes one file on demand to 220x138 JPEG
frames and 16 kHz mono PCM16, and writes WSV2 frames inside an HTTP chunked
response.  The watch only decodes JPEG and plays PCM; it does not decode MP4.
"""

import argparse
import hashlib
import io
import json
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

try:
    import av
    from PIL import Image
except ImportError:
    raise SystemExit("install dependencies: pip install av pillow")

try:
    RESAMPLE = Image.Resampling.LANCZOS
except AttributeError:  # Pillow < 9
    RESAMPLE = Image.LANCZOS

MAGIC = b"WSV2"
HEADER = struct.Struct("<4sBBHHHII")
TYPE_JPEG = 1
TYPE_PCM16 = 2
VIDEO_EXTS = {".mp4", ".mkv", ".mov", ".webm", ".avi", ".m4v", ".ts"}
MEDIA = {}
MEDIA_LOCK = threading.Lock()


def wsv2_message(msg_type, width, height, timestamp, payload):
    header = HEADER.pack(MAGIC, msg_type, 0, width, height, 0,
                         int(timestamp * 1000) & 0xFFFFFFFF, len(payload))
    return header + payload


def scan_media(media_dir):
    found = {}
    for path in media_dir.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in VIDEO_EXTS:
            continue
        key = hashlib.sha1(str(path).encode("utf-8")).hexdigest()[:12]
        found[key] = path
    with MEDIA_LOCK:
        MEDIA.clear()
        MEDIA.update(found)
    return found

def stream_video(handler, path, args):
    container = av.open(str(path))
    video = next((s for s in container.streams if s.type == "video"), None)
    audio = next((s for s in container.streams if s.type == "audio"), None)
    if video is None:
        container.close()
        raise RuntimeError("source has no video stream")
    video.thread_type = "AUTO"
    streams = [video] + ([audio] if audio else [])

    resampler = None
    if audio:
        audio.thread_type = "AUTO"
        resampler = av.AudioResampler(format="s16", layout="mono", rate=16000)

    media_start = None
    wall_start = None
    audio_bytes = max(2, args.audio_ms * 16000 // 1000 * 2)

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

    def chunk(payload):
        if not payload:
            return
        handler.wfile.write(("%X\r\n" % len(payload)).encode("ascii"))
        handler.wfile.write(payload)
        handler.wfile.write(b"\r\n")
        handler.wfile.flush()

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
                    chunk(wsv2_message(TYPE_JPEG, image.width, image.height,
                                       timestamp or 0.0, buf.getvalue()))
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
                            part = pcm[off:off + audio_bytes]
                            if len(part) & 1:
                                part = part[:-1]
                            chunk(wsv2_message(TYPE_PCM16, 0, 0,
                                               timestamp or 0.0, part))
    finally:
        container.close()

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "WatchMedia/1.0"

    def log_message(self, fmt, *args):
        print("%s - %s" % (self.address_string(), fmt % args))

    def authorized(self):
        token = self.server.token
        if not token:
            return True
        return self.headers.get("Authorization", "") == "Bearer " + token

    def send_json(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        if not self.authorized():
            self.send_json(401, {"error": "unauthorized"})
            return

        if parsed.path == "/watch/api/v1/videos":
            items = []
            with MEDIA_LOCK:
                for key, path in MEDIA.items():
                    items.append({
                        "id": key,
                        "title": path.stem,
                        "duration_ms": 0,
                        "width": self.server.width,
                        "height": self.server.height,
                        "stream_url": "/watch/api/v1/videos/%s/stream" % key,
                    })
            self.send_json(200, {"version": 1, "items": items})
            return

        prefix = "/watch/api/v1/videos/"
        if parsed.path.startswith(prefix) and parsed.path.endswith("/stream"):
            key = parsed.path[len(prefix):-len("/stream")]
            with MEDIA_LOCK:
                path = MEDIA.get(key)
            if not path:
                self.send_json(404, {"error": "unknown video"})
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/vnd.wsv2")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                stream_video(self, path, self.server.args)
                self.wfile.write(b"0\r\n\r\n")
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception as exc:
                self.log_message("stream failed: %s", exc)
            return

        self.send_json(404, {"error": "not found"})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--media-dir", default="./videos")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--token", default="")
    parser.add_argument("--width", type=int, default=220)
    parser.add_argument("--height", type=int, default=138)
    parser.add_argument("--quality", type=int, default=72)
    parser.add_argument("--audio-ms", type=int, default=20)
    args = parser.parse_args()

    media_dir = Path(args.media_dir).expanduser().resolve()
    if not media_dir.is_dir():
        raise SystemExit("media directory not found: %s" % media_dir)
    found = scan_media(media_dir)
    print("media dir: %s" % media_dir)
    print("videos: %d" % len(found))
    print("catalog: http://%s:%d/watch/api/v1/videos" %
          (socket.gethostname(), args.port))

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    server.args = args
    server.token = args.token
    server.width = args.width
    server.height = args.height
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
