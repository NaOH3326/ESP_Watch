# ESP_Watch / WatchShell

开发机系统为Archlinux，不对任何windows兼容性做保证

ESP32-S3R8 便携终端的轻量固件（16 MB Flash / 8 MB octal PSRAM）：

| 目录 | 用途 |
|------|------|
| `components/watchshell/` | WatchShell 主固件：转盘、App、Shell、传感器、媒体、游戏、AI 客户端 |
| `main/` | `app_main()` 启动 WatchShell，或按 Kconfig 启动工厂测试 |
| `factorytest/` | 工厂测试组件，REPL 覆盖 PMIC、屏幕、触摸、传感器、音频、Wi-Fi/BLE |
| `components/esp_audio_codec/` | Opus / MP3 / WAV 编解码（esp32s3 预编译库） |
| `components/esp_jpeg/` | JPEG 解码（PICTURES 页） |
| `components/cjson/` | JSON |
| `Doc/reference/` | 从旧小智工程保留下来的 WebSocket / MCP / OTA 协议参考 |
| `server/` | 自建 DeepSeek API / ASR / TTS / 文件 / 视频服务端（FastAPI + Docker） |

服务端快速启动：

```bash
cd server
cp .env.example .env
docker compose up -d --build
```

详见 [`server/README.md`](server/README.md)。

## 0. 获取代码

```bash
git clone https://github.com/NaOH3326/ESP_Watch.git
cd ESP_Watch
```

当前验证的构建环境是 **ESP-IDF v6.0.1**（ESP32-S3 工具链）。仓库已包含音频编解码所需的预编译库，不需要额外下载。

## 1. 编译与烧录

```bash
cd ESP_Watch
source ~/.espressif/tools/activate_idf_v6.0.1.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

- 主控制台：USB-Serial-JTAG（`/dev/ttyACM1`，也就是板载 USB 口）。
- 串口控制台也可用 `esp-watch>` REPL：
  ```text
  status
  wifi <ssid> [password]
  ai <ws://host/path> [token]
  portal
  open sensors
  script 10_sensors.wsh
  rtc [set YYYY-MM-DD HH:MM:SS]
  theme kali|chaos|error|reset
  pmic irq
  idle [seconds, 0=off]
  poweroff
  bootmode fast|normal
  reboot
  # 诊断：录音发送 / 录音回放 / 文本链路 / 模拟触摸 / 导出 RGB565 帧
  ptt [ms]
  echotest [ms]
  chat <text>
  tap <x> <y>
  shot
  ```
- 烧录波特率使用 115200；CH334P Hub 上 460800 曾出现不稳定。

### 工厂测试固件

```bash
idf.py menuconfig
# Factory test -> Enable factory test firmware
idf.py build flash monitor
```

关闭 `CONFIG_FACTORY_TEST_MODE` 后启动 WatchShell；打开后启动工厂测试 REPL。
命令说明见 `factorytest/README.md`。

## 2. 第一次配置

板子第一次启动没有 Wi-Fi 凭据，会自动建立配置热点：

- SSID：`ESP-Watch`
- 密码：`watchshell`
- 浏览器打开：`http://192.168.4.1/`

在网页里可以：

1. 设置 Wi-Fi SSID / 密码（保存后自动重启，切到 STA 模式）。
2. 设置 AI WebSocket URL / token（自建 `xiaozhi-esp32-server` 时填写）。
3. 设置视频目录 URL / token（可选；手表主动拉流时使用，见 `Doc/VIDEO_STREAM.md`）。
4. 设置音乐目录 URL / token（可选；从服务器流式播放 MP3/WAV/AAC/M4A）。
5. 上传 `.wsh` / `.sh` 脚本到 `/storage/scripts/`。
6. 上传 `.mp3` / `.wav` / `.aac` 到 `/storage/music/`。
7. 上传 `.jpg` / `.jpeg`（PNG/WebP 在浏览器里也会转成 JPG）到 `/storage/video/`（Media → PICTURES 显示）；浏览器会先用 Canvas 缩放到最大 440×276 并重新编码为 JPEG，固件只做兼容解码。单个文件最大 6 MB，固件按 4 KB 流式写盘，不会整包占用 RAM。

也可以直接用 USB 串口 REPL 的 `wifi` / `ai` 命令。

## 3. 界面与操作

- 左右滑动：旋转转盘，首尾循环。
- 点按：进入当前 App。
- PWR 短按：转盘进入 / 普通 App 返回；Settings 中执行当前高亮项。
- PWR 长按约 1.2 秒：在任意界面弹出电源菜单（POWER OFF / REBOOT / CANCEL）；LEFT/RIGHT 选择，PWR 短按执行，触摸可直接点按。菜单默认高亮 CANCEL。
- PWR 持续按住 6 秒：AXP2101 硬件关机（即使电源菜单已弹出也会执行）。
- Settings 内：LEFT/RIGHT 移动高亮，PWR 短按执行；按住约 0.7 秒返回；超过 1.2 秒则进入电源菜单；或从左边缘右滑返回。
- GPIO45 / GPIO40：在 App 内翻页/选择；Settings 中用它们移动高亮。
- 从屏幕左边缘向右滑：返回。

### GAMES → MATRIX

- 黑客帝国字符雨效果

### GAMES → HEX

- 二维六边形弹球

### GAMES → FLASK

- 重力水平仪模拟

App：

| App | 内容 |
|-----|------|
| WATCHFACE | 模拟+数字时间、日期、电量、温湿度、步数、航向 |
| AI | 首屏显示连接状态与 `OPEN CHAT`；进入独立聊天页后，按住底部条说话，显示 ASR 回显、DeepSeek 流式文字和 TTS 回复；支持消息滚动与中文 14px 点阵 |
| SHELL | 脚本列表、UP/DOWN/EXEC/STOP、输出滚动、彩色 echo |
| SENSORS | MOTION / COMPASS / ENV / SYSTEM 四页；罗盘 8 秒校准 |
| MEDIA | MUSIC（本地 `/storage/music` 或服务器 `MUSIC CATALOG` 流式播放）、PICTURES（JPG/JPEG，支持浏览、全屏和两次确认删除）、VIDEO（WebSocket/HTTP 视频目录 + 主动拉流；兼容 WSV2 TCP 推送；支持全屏） |
| GAMES | 贪吃蛇、2048、MATRIX 字符雨、HEX 二维六边形弹球、FLASK水平仪，支持触摸滑动/按键 |
| SETTINGS | Wi-Fi、Portal、主题、空闲关机、亮度、音量、关机、重启；按钮加大，支持左右键选择 + PWR 执行 |

主题：

- `KALI`：绿色终端风格（默认）。
- `CHAOS`：彩色故障风格；只给前景着色，绿色点阵背景保持原样。
- `ERROR`：红色故障风格，点阵/扫描线背景也转为红色；隐藏彩蛋；带横向撕裂断层和随机错误窗口，窗口会自动关闭。

在表盘日期区域 **3 秒内连续点按 7 次** 可解锁 `ERROR` 主题

## 3.1 电源 / 低电压 / 启动

- 无操作默认 120 秒自动关机；SETTINGS 的 `IDLE` 可循环 30/60/120/300/OFF。
- 充电中默认不自动关机（`CONFIG_WATCH_IDLE_OFF_WHILE_CHARGING`）。
- 电池低于 3400 mV 且没有 VBUS 时，屏幕显示 3 秒低电压警告后关机；插入 USB 会取消警告。
- 用户主动关机（Settings `POWER OFF` / REPL `poweroff`）后，下一次开机走正常启动。
- 空闲自动关机后，下一次开机走快速启动：先显示 UI，再后台初始化存储/网络。
- `CONFIG_WATCH_*` 可调整空闲时间、低电压阈值、警告秒数和 splash 时间。

### 3.2 网络视频（VIDEO）

- PICTURES 和 VIDEO 页点击画面区域进入全屏（隐藏状态栏/Tab/控件），点屏幕或短按 PWR 退出。
- 全屏画面使用 cover 方式铺满 240×284，超出部分裁切。
- **主动拉流（推荐）**：进入 `MEDIA → VIDEO` 后自动请求视频目录；点条目或 `PLAY` 由手表优先连接该条目的 WebSocket 流（`ws_url`），兼容旧服务器时回退到 HTTP 流（`stream_url`）。`SYNC` 重新取列表，`STOP` 断开。
- 目录 URL 在 Portal 的 `VIDEO CATALOG` 配置，例如 `http://192.168.1.10:8080/watch/api/v1/videos`；token 可选。
- 服务器把 MP4/H.264 转码成 `WSV2`：20 字节帧头 + baseline JPEG 或 `s16le mono 16 kHz` PCM，放在 HTTP chunked body 里。
- 兼容模式仍保留：进入 VIDEO 页后，手表在 TCP `3333` 端口等待发送端。
- 发送端脚本：
  ```bash
  # 主动拉流参考服务器（目录 + 转码 + HTTP 流）
  pip install -r tools/requirements.txt
  python3 tools/watch_media_server.py --media-dir ./videos --port 8080

  # 兼容 TCP 直推
  python3 tools/ws_av_stream.py movie.mp4 --host 192.168.4.1
  ```
- 详细协议见 [`Doc/VIDEO_STREAM.md`](Doc/VIDEO_STREAM.md)。
- 离开 VIDEO 页会停止接收并释放视频帧。

### 3.3 云端音乐（MUSIC）

- 在服务器 Dashboard 上传 `.mp3` / `.wav` / `.aac` / `.m4a`，文件会登记为 `audio`。
- 进入 `MEDIA → MUSIC` 时，手表优先请求 Portal 中配置的 `MUSIC CATALOG`，例如：

  ```text
  http://192.168.1.10:8080/watch/api/v1/music
  ```

- 点条目后手表通过 HTTP 流式读取服务器原始音频字节，不做整首下载；MP3/WAV/AAC/M4A 由手表本地解码后直接写 I2S。
- 服务端只返回手表支持的格式；`.ogg` / `.flac` 等不会出现在目录中。
- 未配置 `MUSIC CATALOG` 或云端目录为空时，自动回退到 `/storage/music` 的本地音乐。
- 底部两排大按钮：上排 `SYNC` / `PLAY` / `STOP`，下排 `VOL-` / `VOL+`；`SYNC` 重新拉取云端目录，`PLAY` 播放选中歌曲，直接点列表行也会播放。
- 目录 URL 和 token 在 Portal 的 `MUSIC CATALOG` 中配置；公网使用 `https://.../watch/api/v1/music`。

## 4. Shell 脚本

脚本放在 `/storage/scripts/*.wsh` 或 `*.sh`，由外部编辑后通过网页上传。Shell 不提供键盘，
只负责选择、执行和滚动输出。

```text
# name: demo
# esc: colored echo + loop
clear
echo green "WATCHSHELL"
echo "inline {red}color{/} works"
set N 3
label loop
echo cyan "tick ${N}"
set N ${N}-1
if N > 0 goto loop
sensors
```

支持：`echo [color]`、内联 `{red}text{/}`、`set`、`sleep`、`if ... goto`、
`label`、`goto`、`clear`、`prompt`、`help`、`info`、`free`、`date`、`batt`、
`sensors`、`wifi scan|status`、`tone`、`volume`、`gpio`、`app open`、`matrix`、
`reboot`、`poweroff`。

示例 `scripts/cmatrix.wsh` 使用 `matrix + sleep + goto` 做循环字符雨；新设备首次启动也会安装 `45_cmatrix.wsh`。真正的全屏长字符串字符雨在 `GAMES → MATRIX`，由固件绘制。

## 5. AI 云端

设备端只实现很小的 WebSocket 客户端，协议参考 `Doc/reference/websocket_zh.md`：

```text
hello / ping / chat(text)
listen start/stop
Opus binary
hello / pong / stt / llm(start|delta|done) / tts / mcp JSON
```

- 默认按住说话（PTT），不做常听唤醒，因此不需要 AEC。
- AI 首屏只作为入口；聊天页才显示 PTT 和消息。PTT 需要按住约 220 ms 才启动录音，普通点按、滑动聊天记录或点击空白区域不会触发空 ASR。
- 服务端先发 `stt` 回显用户识别文本，再发 `llm.delta`（当前完整前缀）流式更新 AI 气泡，`llm.done` 后再播放 TTS；回复长度由 `WATCH_SYSTEM_PROMPT` 和用户请求控制。中文由内置 14px GB2312 点阵显示。
- 聊天页最多持久化约 8 轮（每轮 user + assistant）或 8 KB 文本；当前轮超出单条/总量预算时不上历史，只保留 live 流式显示，下一次说话时丢弃。
- 聊天字库 `components/watchshell/src/watch_font_cjk.c` 由 `tools/gen_cjk_font.py` 从 Noto Sans CJK 生成；修改字号/字形后重新运行脚本即可。
- 服务器建议自建 `xiaozhi-esp32-server` 或本仓库 `server/`；API Key 留在服务器。
- AI URL/token 存 NVS；换服务器不用重新编译。
- 设备端发送 16 kHz / 60 ms Opus，接收 TTS 后按采样率重采样到 16 kHz 播放。
- URL 与子域名/回调的划分见 [`Doc/AI_ENDPOINT.md`](Doc/AI_ENDPOINT.md)：当前是**单条 WebSocket**，功能靠 `type` 字段区分；手表不适合做 HTTP 回调。

## 6. 分区

```text
nvs       0x9000    24 KB
otadata   0xf000     8 KB
phy_init  0x11000    4 KB
factory   0x20000    3 MB
storage   0x320000  13.5 MB  FATFS (/storage)
```

当前固件约 2.0 MB（含 14px GB2312 点阵），3 MB app 分区足够；剩余空间给脚本、音乐、图片。

## 7. 已知限制

- 没有 SD 卡；媒体文件放 FAT 分区或通过网页上传。
- PICTURES 当前是 JPG/JPEG 静态图，不是 MJPEG 连续视频；解码器只支持 baseline JPEG，渐进式 JPEG、HEIC、PNG 需要先转换。
- VIDEO 支持 WebSocket/HTTP 视频目录 + 主动拉流，也兼容自定义 WSV2 发送端（JPEG + PCM16 16 kHz mono）；没有内置摄像头/视频文件解码器，服务器必须转码。
- 中文聊天已内置 14px GB2312 点阵；未覆盖的生僻字/Emoji 仍可能显示为空白。

## 8. 硬件资料

- 引脚表：`main/board_pins.h`
- 网表整理：`Doc/PINMAP.md`
- 旧小智协议参考：`Doc/reference/`
