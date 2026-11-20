# WSV2 网络音视频流协议

MEDIA → `VIDEO` 支持两种模式：

1. **主动拉流（推荐）**：手表先 GET 视频目录，再 GET 选中条目的 HTTP 流。
2. **兼容模式**：手表在 TCP `3333` 端口等待 `tools/ws_av_stream.py` 直接推流。

两种模式使用同一套 `WSV2` 帧格式；手表只做 JPEG 解码、RGB565 显示和 PCM 播放，不做 MP4/H.264 解码。

## 主动拉流

### 1. 视频目录

手表请求配置的 `VIDEO CATALOG URL`，例如：

```http
GET /watch/api/v1/videos HTTP/1.1
Authorization: Bearer <video_token>
X-Device-Id: 34B472xxxxxx
Accept: application/json
```

响应：

```json
{
  "version": 1,
  "items": [
    {
      "id": "a1b2c3d4",
      "title": "Big Buck Bunny",
      "duration_ms": 596000,
      "width": 220,
      "height": 138,
      "stream_url": "/watch/api/v1/videos/a1b2c3d4/stream"
    }
  ]
}
```

- `title` 显示在 VIDEO 页，最多 47 字节。
- `stream_url` 可以是绝对 URL，也可以是以 `/` 开头的同源路径；也可以相对目录 URL。
- 手表一次最多保存 16 条。
- 目录响应上限 48 KB。

### 2. 主动播放

用户点选条目后，手表连接 `stream_url`，请求头带同一个 token：

```http
GET /watch/api/v1/videos/a1b2c3d4/stream HTTP/1.1
Authorization: Bearer <video_token>
X-Device-Id: 34B472xxxxxx
Accept: application/vnd.wsv2, application/octet-stream
```

服务器返回：

```http
HTTP/1.1 200 OK
Content-Type: application/vnd.wsv2
Transfer-Encoding: chunked
Cache-Control: no-store
```

响应体是连续的 `WSV2` 帧（不是 MP4 文件）。推荐使用 chunked，每块可以包含一帧或多帧。手表从 HTTP body 中按 20 字节帧头解析，读到关闭或 STOP 为止。

服务器必须：

- 按 `WSV2` 头部 + payload 连续写；
- 边转码边发送，不能先把整段视频缓冲完；
- 发生客户端断开时停止转码；
- 不提供一个“只有 MP4 文件”的 URL；MP4 由服务器转成 JPEG + PCM。

### 3. 参考服务器

```bash
pip install -r tools/requirements.txt
python3 tools/watch_media_server.py --media-dir ./videos --port 8080
```

然后在手表 Portal 的 `VIDEO CATALOG` 填：

```text
http://<服务器 IP>:8080/watch/api/v1/videos
```

目录会在启动时扫描；新增文件后重启参考服务器。

仓库新增的完整服务端位于 `server/`，同时提供目录、WSV2 转码流、Dashboard 视频播放、文件上传、AI 与 TTS，并保留了本文的目录结构和 WSV2 帧格式。旧的 `tools/watch_media_server.py` 仍是最小参考实现。

## 兼容模式：TCP 直推

MEDIA → `VIDEO` 仍会在 TCP `3333` 端口监听一路发送端。发送端负责采集、缩放、JPEG 编码、音频解码和重采样。参考 `tools/ws_av_stream.py`。

```bash
python3 tools/ws_av_stream.py movie.mp4 --host 192.168.4.1 --port 3333
```

## 帧格式

所有整数为 little-endian，头部固定 20 字节：

| 偏移 | 长度 | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `WSV2` |
| 4 | 1 | type | `1` = JPEG，`2` = PCM16 |
| 5 | 1 | flags | 当前为 0 |
| 6 | 2 | width | JPEG 宽度；PCM 填 0 |
| 8 | 2 | height | JPEG 高度；PCM 填 0 |
| 10 | 2 | reserved | 0 |
| 12 | 4 | timestamp_ms | 发送端媒体时间，毫秒 |
| 16 | 4 | payload_len | 负载长度 |

头部后紧跟 `payload_len` 字节负载，然后可以立即跟下一帧。

### JPEG 帧

- `type=1`。
- 负载为 **baseline JPEG**。
- 推荐尺寸 `220x138`，质量 70 左右，10–15 fps。WatchShell 当前内容区为 204×138；服务端默认输出 204×138 可避免每帧缩放。
- 单帧最大 `256 KB`。
- 手表会在需要时做 1/2、1/4、1/8 降采样兜底，但发送端应自己缩放。

### PCM 帧

- `type=2`。
- 负载为 **signed 16-bit little-endian，mono，16000 Hz**。
- 推荐每帧 20 ms：`16000 * 0.02 * 2 = 640` 字节。
- 单帧最大 `8 KB`。
- 手表通过 I2S 直接播放，不加额外音频解码器。
- 发送端应把音频和视频按时间交错发送；手表收到 PCM 后会写入 I2S，I2S 队列满时会反压 TCP/HTTP。

## 注意

- 协议没有加密和鉴权，只应在可信局域网使用；需要公网时在反向代理上启用 HTTPS + token。
- 手表端只支持 baseline JPEG；渐进式 JPEG 会解码失败。
- 主动拉流当前不支持 seek、断点续传和 Range；STOP 后重新从条目开头播放。
- 发送端必须控制发送节奏。若只发视频不发音频，仍会按发送端时间戳播放。
- 建议先测纯视频，再在发送端加入 PCM，便于区分带宽和解码问题。

