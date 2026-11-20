# AI 端点与 URL 划分

## 1. 当前固件是怎么用 AI URL 的

当前设备只配置一个 URL：

```text
ai_url = wss://host/path
ai_token = Bearer token
```

`watch_cloud.c` 只接受 `ws://` 或 `wss://`，解析出 host、port、path 后建立一条 WebSocket。**URL 只决定“连到哪个服务/路由”，不决定 AI 功能**。功能由 WebSocket 文本 JSON 的 `type` 字段区分：

```text
设备 → 服务：hello / ping / chat(text) / listen(start,stop) / 二进制 Opus
服务 → 设备：hello / pong / stt / llm / tts / mcp / 二进制 TTS Opus
```

因此 `/v1/ws`、`/xiaozhi/v1/`、`/ai` 这些 path 都可以，只要服务端把这条 WS 当成同一个会话通道。**添加子域名不需要改固件**，把 `ai_url` 改成该子域名即可。

## 2. 推荐的 URL 划分

### 2.1 设备实时通道（一条）

```text
wss://rt.example.com/v1/ws
```

这一条连接同时承担：

- 设备 → 服务：语音/文本请求
- 服务 → 设备：STT、LLM、TTS、音频、错误
- 心跳/保活
- 用 `session_id` 或消息 `id` 关联一次请求和它的回调

这比“请求 URL + 回调 URL”简单，因为 WebSocket 本身就是全双工，服务端可以直接在同一连接回推结果。

### 2.2 其他 REST 能力（可选）

```text
https://api.example.com/v1/ai/sessions
https://api.example.com/v1/ai/chat
https://api.example.com/v1/ai/asr
https://api.example.com/v1/ai/tts
```

这些适合手机 App、网页或后台使用，不一定由手表调用。手表目前只实现实时 WS，不实现这些 REST 调用。

### 2.3 视频服务

```text
https://media.example.com/watch/api/v1/videos
https://media.example.com/watch/api/v1/videos/{id}/stream
```

视频目录和视频流与 AI 完全独立，可以放不同子域名、不同机器。

## 3. 子域名可以，但不要按“函数”拆太碎

可以这样：

```text
api.example.com        控制面 REST
rt.example.com         AI WebSocket
media.example.com      视频目录/流
```

不建议：

```text
asr.example.com
tts.example.com
chat.example.com
```

原因：

- 每个子域名都要 DNS 和证书；证书最好用 `*.example.com` 或 SAN。
- 多一次 TLS/DNS 开销，而手表资源有限。
- 功能边界应该由 path + JSON `type` 表达，而不是靠域名。
- 子域名适合“独立部署/独立扩容/独立网关”，不适合表达“同一个会话里的不同消息类型”。

如果确实要拆分：

- `rt.example.com/v1/ws`：实时双向
- `api.example.com/v1/ai/*`：同步请求
- `events.example.com/v1/ai/*`：异步事件回调

三者的 token、证书、CORS 和限流策略要统一。

## 4. “请求与回调”能不能用子域名？

### 4.1 对这台手表：不建议 HTTP 回调

手表运行在 STA（路由器 NAT 后）或 AP 模式，**没有公网可达的 HTTPS 地址**。服务端无法主动 `POST` 到手表；即使有子域名，也缺公网入口、端口映射和证书。

所以设备端应使用：

```text
一条 WSS 长连接
```

服务端在同一个 WSS 上回推结果。这是“请求 + 回调”的等价实现，而且穿透 NAT。

### 4.2 如果后台确实需要异步 Job

可以让**后台/手机**使用请求 + 回调：

```http
POST https://api.example.com/v1/ai/jobs
Content-Type: application/json

{"prompt":"...", "callback_url":"https://backend.example.com/hooks/ai"}
```

```http
202 Accepted
{"job_id":"job_123", "status":"queued"}
```

完成后服务端：

```http
POST https://backend.example.com/hooks/ai
{"job_id":"job_123", "status":"done", "result":{...}}
```

这条链路里，回调地址是**后台服务器**，不是手表。手表只订阅 WSS 事件，或者轮询一个查询接口：

```text
GET https://api.example.com/v1/ai/jobs/job_123
```

### 4.3 如果非要从服务端推到手表

需要额外的公网入口，例如：

- MQTT（手表作为客户端订阅主题）——比 HTTP 回调更适合 NAT；
- 长轮询；
- 自建反向隧道；
- 公网网关把消息转成 WebSocket 下发。

HTTP 回调到手表本身不现实。

## 5. 结论

1. 当前 `ai_url` 是**一个 WebSocket 端点**，功能靠消息 `type` 划分。
2. 加 `rt.example.com` 这类子域名完全可以，只要证书覆盖。
3. 不要用“每个 AI 功能一个子域名”的方式划分；用 path + `type`。
4. 手表不要设计 HTTP 回调；用单条 WSS 全双工。
5. 后台异步任务可以使用 `api.example.com` 请求 + `backend.example.com` 回调；手表只通过 WSS 收事件，或轮询结果。

## 6. 仓库中的参考实现

`server/` 提供一套可直接部署的实现：

```text
ws(s)://host/v1/ws          AI 实时通道（hello / listen / STT / LLM / TTS / Opus）
GET      /watch/api/v1/videos         视频目录
GET      /watch/api/v1/videos/{id}/stream  WSV2 流
POST     /api/chat                     Dashboard SSE
POST     /api/tts                      Piper/CosyVoice WAV
```

AI 后端固定使用 DeepSeek API（`DEEPSEEK_API_KEY`）；手表语音输入通过 OpenAI-compatible ASR 服务完成，可本地运行 Speaches（faster-whisper）或使用云端 API，详见 `server/README.md`。空音频不会直接丢弃，而是把“用户回复了空消息”作为合成用户消息交给 DeepSeek，由 AI 生成回复并走 TTS。

## 7. 手表聊天页的消息流

手表 AI 首屏只显示连接状态和 `OPEN CHAT` 入口；进入聊天页后才显示 PTT、ASR 回显和 AI 文字流。服务端一次语音/文本请求按下面的顺序发送：

```text
stt  { "type":"stt", "text":"用户说的话" }
llm  { "type":"llm", "state":"start", "text":"" }
llm  { "type":"llm", "state":"delta", "text":"当前已生成的前缀" }
...
llm  { "type":"llm", "state":"done",  "text":"最终显示文本" }
tts  { "type":"tts", "state":"start" }
... 二进制 Opus 音频 ...
tts  { "type":"tts", "state":"stop" }
```

- `delta` 是**当前完整前缀**，不是增量片段；手表直接替换当前 AI 气泡，避免丢包/重连后拼接错位。服务端约每 80 ms 节流发送一次，文字先出现，随后才播放 TTS。
- 服务端不再按固定字数截断回复；长度由 `WATCH_SYSTEM_PROMPT` 和用户请求控制，`done` 发送完整文本。
- 手表最多持久化约 8 轮（user + assistant）或 8 KB 文本；当前轮超出预算时仍 live 流式显示，但不写入历史，下一次说话时丢弃。
- 聊天页支持上下拖动查看历史，`CLR` 只清空本地显示（含 live 轮），不删除服务端上下文。
