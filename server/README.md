# ESP_Watch Server

一个可独立部署的 ESP_Watch 服务端项目，面向低性能 Linux 主机、NAS、树莓派或小 VPS。

它同时提供：

- **AI 对话**：DeepSeek 官方 API（OpenAI-compatible，不再运行本地 LLM）；
- **ASR**：可选 OpenAI-compatible 语音识别；支持本地 Speaches（faster-whisper）或云端；
- **TTS**：默认 Piper 本地合成，可选 CosyVoice HTTP 适配器；
- **Dashboard**：AI 对话、TTS 试听、文件上传、视频播放、运行日志、运行状态；
- **文件上传**：流式写盘、分类、Range 下载、删除；
- **手表视频**：`VIDEO CATALOG` 返回 `ws_url`；手表优先走 WebSocket（JPEG+PCM 每帧一条 binary message），`stream_url` 保留 HTTP WSV2 兼容回退；
- **手表 AI**：兼容固件的 `/v1/ws` WebSocket 协议（hello / listen / STT / LLM / TTS / Opus）。

## 1. 架构

```text
ESP32 Watch ── WSS /v1/ws ─────────────┐
      │                                 │
      ├─ HTTP /watch/api/v1/videos ─────┤
      └─ WSS /watch/api/v1/videos/*/ws ─┤
                                        ▼
                              FastAPI (single process)
                              ├─ SQLite metadata
                              ├─ AI: DeepSeek API
                              ├─ TTS: Piper / CosyVoice HTTP
                              ├─ ASR: OpenAI-compatible API
                              └─ files/  +  WSV2 transcoder (PyAV)

Browser Dashboard ── HTTP ──────────────┘
```

默认只需要 **1 个 FastAPI 进程 + 1 个 SQLite 文件**。视频按需转码，不预生成 HLS，不依赖 Redis/PostgreSQL/Node.js。

## 2. 快速开始（Docker，推荐）

```bash
cd server
cp .env.example .env
# 编辑 .env：至少改 ADMIN_TOKEN、WATCH_TOKEN，并填 DEEPSEEK_API_KEY。
docker compose up -d --build
```

打开：

```text
http://<服务器 IP>:8080
```

如果 8080 已被其他服务占用，不要停现有服务；在 `.env` 中改宿主端口：

```dotenv
# 只让本机/Caddy/SSH 隧道访问，避免占用公网 8080
WATCH_BIND=127.0.0.1
WATCH_PORT=18080
```

然后 `docker compose up -d --no-build`，Dashboard 地址变为：

```text
http://127.0.0.1:18080
```

如果直接暴露到局域网测试，可以把 `WATCH_BIND` 改回 `0.0.0.0`，并使用安全组放行对应端口。

Piper 声音默认在容器首次启动时自动下载到 `./data/models/piper`。如果主机不能访问 Hugging Face，可以先手动下载，或设置 `PIPER_URL` 指向已有的 Piper HTTP 服务。

查看日志：

```bash
docker compose logs -f watch-server
```

Dashboard 的 **运行日志** 页面也会实时显示 WebSocket、ASR、LLM、TTS 日志，适合不打开终端时排查手表问题。

停止：

```bash
docker compose down
```

### 国内网络环境

Dockerfile 默认已经把两处最容易卡住的源改成国内镜像：

- base image 的 Debian apt 源：`mirrors.aliyun.com`
- pip 源：`pypi.tuna.tsinghua.edu.cn`

如果你的 Docker Hub 拉取 `python:3.12-slim` 很慢，先在阿里云容器镜像服务控制台获取专属加速地址，然后配置：

```bash
sudo mkdir -p /etc/docker
sudo tee /etc/docker/daemon.json >/dev/null <<'EOF'
{
  "registry-mirrors": ["https://<你的ID>.mirror.aliyuncs.com"]
}
EOF
sudo systemctl daemon-reload
sudo systemctl restart docker
```

> 重启 Docker 可能影响这台机器上的其他容器；也可以在低峰期操作。

注意：`docker compose build` 默认使用 BuildKit。部分 Docker 版本里，`daemon.json` 的 `registry-mirrors` 只对 `docker pull` 生效，**不会自动作用于 Dockerfile 的 `FROM` 拉取**。如果构建日志长时间停在：

```text
[1/9] FROM docker.io/library/python:3.12-slim
```

可以先从加速器拉基础镜像，并直接把 `BASE_IMAGE` 指向镜像地址：

```bash
docker pull <你的ID>.mirror.aliyuncs.com/library/python:3.12-slim

cd ~/ESP_Watch/server
docker compose build \
  --build-arg BASE_IMAGE=<你的ID>.mirror.aliyuncs.com/library/python:3.12-slim
```

验证 `docker pull` 和 `docker compose build` 的差异：

```bash
# 如果这一条很快，说明镜像加速器本身没问题，问题在 BuildKit
time docker pull python:3.12-slim
```

如果镜像加速器也没有 Python 基础镜像，或者服务器带宽仍然太差，直接使用下面的“本机构建后传镜像”方案。

国内服务器不建议让 Piper 自动从 Hugging Face 下载。设置：

```dotenv
AUTO_DOWNLOAD_PIPER=0
```

在本机从 `hf-mirror.com` 下载 `zh_CN-huayan-medium`：

```bash
wget -L -c \
  "https://hf-mirror.com/rhasspy/piper-voices/resolve/main/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx?download=true" \
  -O zh_CN-huayan-medium.onnx
wget -L -c \
  "https://hf-mirror.com/rhasspy/piper-voices/resolve/main/zh/zh_CN/huayan/medium/zh_CN-huayan-medium.onnx.json?download=true" \
  -O zh_CN-huayan-medium.onnx.json
```

然后在**本机**建立 SSH 隧道，打开 Dashboard 上传模型：

```bash
ssh -N -L 18080:127.0.0.1:8080 -p 22236 naoh@121.199.23.188
```

浏览器访问 `http://127.0.0.1:18080`，到 **TTS 语音 → Piper 模型管理** 上传 `.onnx` 和 `.onnx.json`。这样服务器完全不需要访问 Hugging Face，也不需要把 8080 暴露到公网。

如果服务器构建镜像时内存不够，可以在本机构建后传镜像：

```bash
# 本机
cd server
docker compose build
docker save server-watch-server:latest | gzip > /tmp/watch-server.tar.gz
scp -P 22236 /tmp/watch-server.tar.gz naoh@121.199.23.188:/tmp/

# 服务器
gunzip -c /tmp/watch-server.tar.gz | docker load
cd ~/ESP_Watch/server
docker compose up -d --no-build
```

### 使用 DeepSeek

AI 对话现在**只有 DeepSeek**，不再需要 Ollama；服务端不加载本地 LLM。

在 `.env` 中设置：

```dotenv
DEEPSEEK_API_KEY=sk-...
DEEPSEEK_BASE_URL=https://api.deepseek.com
DEEPSEEK_MODEL=deepseek-chat
```

Dashboard 固定使用 `DeepSeek API`，只保留模型名输入。手表可直接使用：

```text
wss://your-domain/v1/ws
```

`?provider=deepseek&model=deepseek-chat` 仍兼容，但不再是必需参数。

DeepSeek 是文本模型，**不提供 ASR**。手表语音输入需要一个 OpenAI-compatible ASR 服务；可以直接用第 6.2 节的本地 Speaches，也可以配置云端 API。不配置 ASR 时聊天页仍可打开，但按住说话会显示 ASR 未配置错误。

## 3. 本地运行（不用 Docker）

要求 Python **3.11 / 3.12**（推荐 3.12）。`PyAV`、`onnxruntime` 对最新 Python 版本的 wheel 可能滞后。

```bash
cd server
./scripts/setup_tts.sh
cp .env.example .env   # setup_tts.sh 已自动创建时跳过
# 编辑 .env
./scripts/run.sh
```

`setup_tts.sh` 会：

1. 创建 `.venv`；
2. 安装 `requirements.txt` + `requirements-tts.txt`；
3. 下载 `zh_CN-huayan-medium` Piper 声音。

如果不需要 TTS，也可以只安装：

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
TTS_PROVIDER=none ./scripts/run.sh
```

## 4. 手表配置

在手表 Portal 中填写：

```text
AI URL:          ws://<服务器 IP>:8080/v1/ws
AI TOKEN:        .env 中的 WATCH_TOKEN

VIDEO CATALOG:   http://<服务器 IP>:8080/watch/api/v1/videos
VIDEO TOKEN:     .env 中的 WATCH_TOKEN
```

公网部署请使用 HTTPS/WSS：

```text
AI URL:          wss://watch.example.com/v1/ws
VIDEO CATALOG:   https://watch.example.com/watch/api/v1/videos
```

`deploy/Caddyfile` 提供了反向代理模板。Caddy 会自动申请证书，并把 SSE / WSV2 的 chunked 响应及时刷新。

## 5. TTS

### Piper（默认，推荐低配）

```dotenv
TTS_PROVIDER=piper
PIPER_VOICE=zh_CN-huayan-medium
PIPER_MODEL_DIR=./data/models/piper
AUTO_DOWNLOAD_PIPER=1
TTS_CACHE=1
```

- Piper 是 CPU 推理，适合 1–2 vCPU 小机器；
- 首次请求加载模型，后续复用；
- Dashboard 的 TTS 面板输出 WAV，可直接播放或下载；
- 手表 TTS 会由服务器重采样到 24 kHz、编码成 60 ms Opus 帧。

手动下载其他声音：

```bash
. .venv/bin/activate
python -m piper.download_voices --help
python -m piper.download_voices zh_CN-huayan-medium --data-dir ./data/models/piper
```

### 面板上传 Piper 模型（推荐）

打开 Dashboard 的 **TTS 语音 → Piper 模型管理**，一次上传：

- `*.onnx`（模型本体）
- `*.onnx.json`（同名的配置文件）

上传后会立即出现在声音下拉框，并且默认设为手表 TTS 使用的当前声音。活动声音写入 `data/tts-voice.txt`，模型与配置保存在 `data/models/piper/`，重启容器不会丢失。

同名模型默认拒绝覆盖；勾选“覆盖同名模型”才会替换。新模型最大 256 MB，可用 `MAX_TTS_MODEL_MB` 调整。也可以直接调用接口：

```bash
curl -H "Authorization: Bearer $ADMIN_TOKEN" \
  -F "file=@zh_CN-huayan-medium.onnx" \
  -F "config=@zh_CN-huayan-medium.onnx.json" \
  -F "name=zh_CN-huayan-medium" \
  -F "activate=1" \
  http://127.0.0.1:8080/api/tts/voices
```

如果拿到的是 Piper 语音压缩包，先在本地解压，再上传其中的 `.onnx` 和 `.onnx.json`；不要只上传 `.onnx`，否则服务端会返回“缺少 Piper config”。

也可以在宿主机直接放文件（Docker 的数据目录是 `server/data/models/piper/`）：

```bash
scp -P 22236 zh_CN-huayan-medium.onnx      naoh@121.199.23.188:~/ESP_Watch/server/data/models/piper/
scp -P 22236 zh_CN-huayan-medium.onnx.json naoh@121.199.23.188:~/ESP_Watch/server/data/models/piper/
```

然后在面板刷新模型并选择声音即可。若设置了 `PIPER_MODEL` 指向某个固定 `.onnx` 文件，它会优先于面板选择；一般不要设置，让 Dashboard 管理。

也可以在 `PIPER_URL` 指向已运行的 Piper HTTP 服务：

```bash
python -m piper.http_server -m zh_CN-huayan-medium --port 5000
```

### CosyVoice（可选，需要外部服务）

CosyVoice 通常需要 GPU，且各项目的 HTTP API 不统一，因此本项目只提供明确的适配契约：

```dotenv
TTS_PROVIDER=cosyvoice
COSYVOICE_URL=http://127.0.0.1:50000/tts
COSYVOICE_SPEAKER=中文女
COSYVOICE_TOKEN=
```

服务端会发送：

```json
{"text": "你好", "speaker": "中文女", "format": "wav"}
```

并期望响应为 **WAV 二进制**；如果返回 JSON，也支持 `audio` 字段为 base64 WAV。若你的 CosyVoice 部署接口不同，改 `app/tts.py` 的 `_synthesize_cosyvoice` 即可。

## 6. ASR（手表语音输入）

固件 PTT 发送的是 Opus。服务端会先解码为 16 kHz mono WAV，再调用一个 OpenAI-compatible `/audio/transcriptions` 服务。这个服务可以是用本机 Speaches 跑的 faster-whisper，也可以是云端 API。

### 6.1 配置

```dotenv
ASR_PROVIDER=openai_compatible
ASR_BASE_URL=https://api.openai.com/v1
ASR_API_KEY=sk-...
ASR_MODEL=whisper-1
ASR_LANGUAGE=zh
ASR_PROMPT=
ASR_TIMEOUT_S=60
```

`ASR_LANGUAGE=zh` 可选；部分兼容服务会拒绝额外字段，此时留空。`ASR_PROMPT` 是可选词表提示，例如人名/地名。

### 6.2 本地运行（无需 ASR 服务商）

推荐用 **Speaches**：它内部跑 faster-whisper，但对外提供 OpenAI-compatible `/v1/audio/transcriptions`，所以后端不用改代码。

```bash
cd server
docker compose --profile asr up -d
```

Compose 会启动 `esp-watch-asr`，模型缓存到 Docker volume `server_asr-cache`。检查：

```bash
curl http://127.0.0.1:18000/v1/models
docker compose logs -f speaches
```

如果 `/v1/models` 返回空列表，手动触发首次下载：

```bash
curl -X POST \
  http://127.0.0.1:18000/v1/models/Systran/faster-whisper-small
```

> **不要让模型按 TTL 卸载。** 当前 Speaches 版本读取的是 `WHISPER__TTL`，不是 `STT_MODEL_TTL`。Compose 已设置 `WHISPER__TTL=-1`，模型会常驻内存；否则空闲 5 分钟后模型被卸载，下一次 PTT 会重新访问 Hugging Face，表现为 ASR 长时间无响应或 `ASR request failed`。

然后在 `.env` 中配置：

```dotenv
ASR_PROVIDER=openai_compatible
ASR_BASE_URL=http://speaches:8000/v1
ASR_API_KEY=
ASR_MODEL=Systran/faster-whisper-small
ASR_LANGUAGE=zh
ASR_TIMEOUT_S=120
```

重启 watch-server：

```bash
docker compose --profile asr up -d
curl -s http://127.0.0.1:8080/api/status | python3 -m json.tool
```

`asr.available` 应为 `true`，仓库默认的 `faster-whisper-small` 是 multilingual 模型，支持中文。

资源建议：

- `small`：约 1–2 GB 内存，2 vCPU 可跑，适合短 PTT；
- 中文效果更好可把 Compose 里的 `PRELOAD_MODELS` 和 `.env` 的 `ASR_MODEL` 一起换成 `Systran/faster-whisper-medium`，但内存/延迟更高；
- 宿主机调试端口只绑定在 `127.0.0.1:18000`；不要改成公网监听。Speaches 默认无鉴权，只给内网 watch-server 使用。

### 6.3 使用云端 ASR

| 服务 | `ASR_BASE_URL` | `ASR_MODEL` |
|---|---|---|
| OpenAI | `https://api.openai.com/v1` | `whisper-1` 或 `gpt-4o-mini-transcribe` |
| Groq | `https://api.groq.com/openai/v1` | `whisper-large-v3-turbo` |
| SiliconFlow | `https://api.siliconflow.cn/v1` | `FunAudioLLM/SenseVoiceSmall` |

其他接受 `multipart/form-data`（`file` + `model`）并返回 `{"text":"..."}` 的服务也可以直接使用。DeepSeek 本身没有 ASR，不能把 `ASR_BASE_URL` 指向 DeepSeek。

配置后 `/api/status` 的 `asr.available` 会变为 `true`，Dashboard 运行状态会出现 `ASR READY`。

### 6.4 空语音与超时

- 手表松开 PTT 但没有音频包，或 ASR 返回空文本时，服务端不会直接用固定文案回复；
- 它会把一条合成的用户消息交给 DeepSeek，例如“用户刚刚按下了语音键，但没有检测到语音内容（空音频）……”；
- 手表先显示 `stt: （未检测到语音）`，随后显示并播报 AI 生成的回复；
- 可通过 `EMPTY_ASR_PROMPT` 修改交给 AI 的提示；
- 固件松手会立即发送 `listen:stop`；如果服务端在 `PTT_MAX_S`（默认 30 秒）内只收到 `start`，会自动处理已有音频或按空消息交给 AI；
- 固件端另有 45 秒看门狗，若仍没有 `stt/llm/tts` 响应会从 `THINKING` 切到 `ERROR: response timeout`。

### 6.5 没有 ASR 时

- Dashboard 文字对话正常；
- 手表聊天页可打开，按住说话会收到 `ASR is not configured`；
- 测试时可以通过 `{"type":"listen","state":"stop","text":"你好"}` 直接走文本链路，不经过 ASR。

## 7. Dashboard

| 页面 | 能力 |
|---|---|
| AI 对话 | DeepSeek API，流式输出，多轮上下文 |
| TTS 语音 | 输入文本、选声音、生成 WAV、试听、下载；上传/切换/删除 Piper 模型 |
| 文件上传 | 拖拽、多文件、进度、分类筛选、删除 |
| 视频播放 | 浏览器直接 Range 播放 MP4/WebM |
| 运行日志 | 轮询 `/api/logs`，显示 WebSocket / ASR / LLM / TTS 日志 |
| 运行状态 | DeepSeek/TTS/ASR 可用性、媒体计数、配置快照 |

如果 `.env` 设置了 `ADMIN_TOKEN`，第一次访问会弹出 Token 输入框，Token 只保存在浏览器 `localStorage`。


## 8. API 一览

### Dashboard / 管理

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/status` | 公开状态与安全配置快照 |
| GET | `/api/logs` | Dashboard 运行日志（需要 Admin Token） |
| GET | `/api/ai/providers` | DeepSeek 可用性 |
| POST | `/api/chat` | 流式 SSE 或 `stream:false` JSON |
| GET | `/api/tts/voices` | Piper/CosyVoice 声音，含大小/配置/默认状态 |
| POST | `/api/tts/voices` | multipart 上传 Piper `.onnx` + `.onnx.json` |
| POST | `/api/tts/voices/{voice}/select` | 设为手表默认 TTS 声音 |
| DELETE | `/api/tts/voices/{voice}` | 删除本地 Piper 声音 |
| POST | `/api/tts` | 合成 WAV |
| GET | `/api/files?kind=video` | 文件列表 |
| POST | `/api/files` | multipart 上传，字段 `file` / `kind` / `title` |
| DELETE | `/api/files/{id}` | 删除 |
| GET/HEAD | `/api/files/{id}/content` | 支持 `Range` |

`ADMIN_TOKEN` 通过 `Authorization: Bearer ...`、`X-Admin-Token` 或 `?token=` 传递。视频 `<video>` 标签不能自定义 Header，因此 Dashboard 在 URL 上带 `?token=`。

### 手表

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/watch/api/v1/videos` | 最多 16 条目录，标题截断 47 字节；包含 `stream_url` 和 `ws_url` |
| GET | `/watch/api/v1/videos/{id}/stream` | `application/vnd.wsv2` chunked 流（HTTP 兼容回退） |
| WS | `/watch/api/v1/videos/{id}/ws` | WSV2 音视频 WebSocket 流；每条 binary message 一个帧 |
| WS | `/v1/ws`、`/ai` | AI 实时通道 |

`WATCH_TOKEN` 为空时允许无鉴权局域网访问；公网部署务必设置。

WS 消息与固件 `Doc/reference/websocket_zh.md` 兼容：

```text
设备 → 服务：hello / ping / chat(text) / listen / 二进制 Opus / abort
服务 → 设备：hello / pong / stt / llm(start|delta|done) / tts(start|sentence_start|stop) / 二进制 Opus
```

`llm.delta` 是当前完整前缀，用于手表聊天页实时显示；`llm.done` 是最终文本。之后服务端才开始发送 TTS Opus 帧。回复长度由 `WATCH_SYSTEM_PROMPT` 和用户请求控制，服务端不再按固定字数截断；手表最多持久化约 8 轮/8 KB，超出预算的当前轮只在聊天页 live 显示，不进入历史。

## 9. 目录与数据

```text
server/
  app/
    main.py        FastAPI 路由
    ai.py          DeepSeek API
    tts.py         Piper / CosyVoice
    asr.py         OpenAI-compatible ASR API
    audio.py       WAV/PCM/Opus
    wsv2.py        WSV2 转码
    media.py       上传与媒体探测
    ws_ai.py       手表 WebSocket 会话
    static/        Dashboard
  scripts/         setup_tts.sh / run.sh
  deploy/Caddyfile
  data/            SQLite、上传文件、Piper 模型、TTS 缓存（gitignored）
```

默认数据目录：

```text
data/watch.db
data/files/
data/models/piper/
data/tts-voice.txt     # Dashboard 选中的默认 Piper 声音
data/tts-cache/
```

服务端启动时会扫描 `data/files/`，把 SQLite 中没有记录的文件重新登记（适合数据库丢失、旧文件迁移或上传中断恢复）；没有原元数据时标题会回退为文件名。

## 10. 低性能部署建议

- **最低**：1 vCPU / 1 GB RAM，Dashboard 文字 + DeepSeek API（不在本机跑 LLM）+ Piper 小模型；
- **推荐**：1–2 vCPU / 1–2 GB RAM，DeepSeek API + Piper；ASR 使用本地 Speaches 时额外占用 1–2 GB 内存；只用云端 ASR 则不占本机算力；
- **视频**：同一时间只建议 1 路 WSV2 转码；WSV2 是 CPU 解码 + JPEG 编码，204×138 / 12 fps 的开销远低于实时 1080p；
- 如果手表在**有音频**的视频上掉帧/卡死，先设置 `WATCH_VIDEO_AUDIO=0` 验证纯视频；同时建议更新固件：新 `watch_video.c` 优先走 WebSocket，PCM 使用 16 帧独立队列 + 高优先级 `video_aud` 任务，并对 `watch_audio_init()` 加锁，避免视频后 I2S 初始化失败；
- **不要**把上传目录放在内存盘；视频文件可能很大；
- **反向代理**：Caddy 配置已设置 `flush_interval -1`，否则 SSE 和 WSV2 会被缓冲；
- **Swap**：Piper 首次加载/视频转码可能短暂占用内存；如果仍在本机跑 TTS，低配机器建议 1 GB swap。

## 11. 安全

- 修改 `.env` 中两个默认 Token；
- DeepSeek Key 只存在服务端 `.env`，不会返回给 Dashboard；
- 上传文件使用随机 ID 与随机存储名，原文件名只作为元数据；
- 静态文件与下载响应带 `X-Content-Type-Options: nosniff`；
- 非媒体附件以 `attachment` 下载，避免在浏览器中执行；
- 公网建议只暴露 443，由 Caddy/Nginx 终止 TLS；8080 只监听内网。

## 12. 测试

```bash
cd server
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt -r requirements-dev.txt
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 pytest -q
```

Docker 内测试：

```bash
docker build -t esp-watch-server:test .
docker run --rm -e AUTO_DOWNLOAD_PIPER=0 -e PYTHONPATH=/src \
  -v "$PWD:/src" -w /src esp-watch-server:test \
  sh -c 'pip install -q pytest && PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 pytest -q'
```

## 13. 已知限制

- `WSV2` 需要 `PyAV`；核心 Dashboard 和上传不依赖它，但手表视频流会返回 503；
- Dashboard 视频不做转码，浏览器不支持的容器/编码（如部分 MKV）需要先转成 MP4/H.264；
- CosyVoice 不随项目安装，需要外部 HTTP 服务；
- DeepSeek 没有 ASR；手表语音识别可以使用本地 Speaches，或单独配置云端 ASR API；
- Piper 首次模型加载有延迟，生产环境可先调用一次预热；
- 单进程 SQLite 适合个人/小团队；多实例水平扩展需要换 PostgreSQL 和共享存储。

## 14. 与仓库现有工具的关系

`tools/watch_media_server.py` 是更早的参考 WSV2 服务器；本项目在它基础上增加了 Dashboard、鉴权、SQLite 文件管理和 AI/TTS，并保持相同的目录和 WSV2 协议。原有 `tools/ws_av_stream.py` 仍可用于 TCP 直推兼容模式。

## 15. 故障排查

### 手表 AI 连不上，但视频正常

视频正常说明 IP、端口、HTTP 和 token 基本可达；AI 走的是 **WebSocket 升级**，需要 Uvicorn 安装 `websockets`：

```text
requirements.txt: websockets>=12,<16
```

旧镜像如果是在加入该依赖前构建的，HTTP/视频能工作，但 `/v1/ws` 无法升级。执行：

```bash
cd server
docker compose up -d --build
docker compose logs -f watch-server
```

手表发起连接后，日志应出现：

```text
watch AI websocket connected: client=... provider=...
```

如果看到 `watch AI websocket rejected (bad token)`，检查 Portal 里的 TOKEN 是否与 `.env` 的 `WATCH_TOKEN` 完全一致。URL 使用：

```text
ws://<服务器 IP>:8080/v1/ws
```

不要在末尾多写 `/`，也不要把 HTTP 视频目录 URL 填到 AI URL。

连接成功但按住说话后提示 `ASR is not configured`，说明 WebSocket 和 AI 已通，但语音输入还缺 ASR；按第 6 节配置一个 OpenAI-compatible ASR 服务即可。Dashboard 的文字对话不依赖 ASR。

### 视频显示 9 FPS / VIDEO 页面偶发卡死

旧固件在 WSV2 拉流任务里同步执行 `watch_audio_write()`，20 ms 音频会阻塞约 20 ms，等于把拉流线程当作音频播放时钟，挤压 JPEG 解码和网络读取。当前 `watch_video.c` 已改为：

```text
WebSocket/HTTP 拉流任务 → 16 帧 PCM 队列 → 独立高优先级 video_aud 任务 → I2S
```

WebSocket 模式下每条 binary message 是一个完整的 WSV2 帧，JPEG 和 PCM 在同一条连接中交错下发；旧服务端仍可回退 HTTP。

需要重新编译并烧录固件：

```bash
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

如果暂时不能更新固件，可以先用纯视频验证：

```dotenv
WATCH_VIDEO_AUDIO=0
```

然后重启服务端。纯视频仍卡死时，再降低：

```dotenv
WATCH_VIDEO_FPS=10
WATCH_VIDEO_WIDTH=200
WATCH_VIDEO_HEIGHT=125
WATCH_VIDEO_QUALITY=65
```

FPS 是手表端按“成功解码的 JPEG 帧”统计的，不是服务器发送值；音频阻塞、JPEG 解码或 UI blit 都会让它降低。

