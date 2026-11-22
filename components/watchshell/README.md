# WatchShell

轻量 ESP32-S3 手表固件，入口 `watchshell_start()`。

## 结构

```text
include/
  watchshell.h       公共入口
  watch_types.h      UI/事件/颜色
  watch_gfx.h        帧缓冲、线条、文字、图标
  watch_input.h      CST816D + 按键事件
  watch_term.h       彩色终端缓冲
  watch_config.h     NVS 配置
  watch_storage.h    FATFS /storage
  watch_net.h        Wi-Fi + 配置/上传 HTTP 服务
  watch_audio.h      I2S 16 kHz 全双工
  watch_cloud.h      WebSocket + Opus AI 客户端
  watch_script.h     .wsh 执行器
  watch_media.h      MP3/WAV 播放
  watch_power.h      空闲关机 / 低电压 / 启动模式
  watch_theme.h      KALI / CHAOS / ERROR 主题
  watch_ui.h         转盘和 App 框架
  watch_apps.h       App 接口
  watch_console.h    USB-Serial-JTAG REPL
src/                 对应实现 + app_*.c
```

## 线程

| 任务 | 核 | 作用 |
|---|---|---|
| `watch_ui` | 1 | 输入、转盘、App 渲染，约 30 fps |
| `sensors` | 0 | IMU / 磁力计 / AHT20 / RTC 缓存 |
| `ai_ws` | 0 | WebSocket 收包 |
| `ai_cap` | 1 | PTT 麦克风 Opus 编码 |
| `ai_play` | 1 | TTS Opus 解码 + I2S 输出 |
| `wsh` | 1 | 脚本执行 |
| `media` | 1 | MP3/WAV/AAC 解码，本地或云端 HTTP 流式音乐 |
| `video` | 0 | WSV2 TCP 推送 + 目录 WebSocket/HTTP 拉流，JPEG 解码后给 UI |
| `boot_svc` | 0 | 快速启动时的存储 / 网络后台初始化 |
| `httpd` | 任意 | 配置 / 上传网页 |

## UI

转盘轴心在屏幕下方。App 位置由 `s_pos` 控制，图标以
`(-90° + offset*50°)` 绕 `(120, 430)` 旋转；当前 App 在 `(120,130)`，
左右相邻 App 大部分在屏幕外，因此一个图标占一屏。滑动改变 `s_pos`，
松手吸附到整数索引；点击进入 App。

转盘点阵背景铺满状态栏以下的整屏，底部脚注区降低点密度，并用两段亮端 + 暗中段的细分隔线与小字/页码分开，不再在 `y=250` 处硬截断。

## 新增 App

在 `app_*.c` 中实现 `ws_app_impl_t`，加入 `watch_ui.c` 的 `s_apps[]`
和 `watch_types.h` 的 `ws_app_id_t`，再在 `CMakeLists.txt` 添加源文件。
`render()` 负责内容区域；顶部 22 px 状态栏由 UI 统一绘制。

## 媒体

- PICTURES 枚举 `/storage/video` 下的 `.jpg` / `.jpeg`，触摸列表或 LEFT/RIGHT 选择图片。
- JPEG 先读入 PSRAM，按 1/1 → 1/2 → 1/4 → 1/8 依次尝试解码，输出控制在 1 MB 内，避免手机大图耗尽 PSRAM。
- PICTURES 页底部 `DELETE` 需要连续点按两次（第二次显示 `CONFIRM`）才删除当前图片；列表每 1.5 秒自动刷新。
- PICTURES/VIDEO 点击画面进入全屏：隐藏状态栏和 MEDIA 控件，按 cover 方式铺满 240×284；点屏幕或短按 PWR 退出。
- 网页上传改为 4 KB 分块流式写盘，单文件上限 6 MB，并先写隐藏临时文件，完成后 `rename`，避免上传中断留下半张图。
- 网页在浏览器侧用 Canvas 把图片缩放到最大 440×276 并重新编码 JPEG；固件保留降采样兜底，兼容旧的未压缩大图。
- `VIDEO` 支持 **目录 + WebSocket 主动拉流**：进入页后 GET 配置的目录，点条目或 `PLAY` 优先连接该条目的 `ws_url`（每条 binary message 是一个 WSV2 帧），旧目录没有 `ws_url` 时回退 HTTP body；`SYNC` 刷新、`STOP` 断开。目录在 Portal 的 `VIDEO CATALOG` 配置。
- 兼容模式仍是 TCP `3333` 接收端，协议 `WSV2`：20 字节头 + baseline JPEG 或 PCM16。发送端负责缩放/编码；PCM 为 `s16le mono 16 kHz`，手表直接写 I2S。参考 `tools/ws_av_stream.py`、`tools/watch_media_server.py` 和 `Doc/VIDEO_STREAM.md`。
- WSV2 的 PCM 不再在拉流任务里同步写 I2S：`watch_video.c` 使用 16 帧队列 + 独立 `video_aud` 任务播放，并对音频初始化加锁；WebSocket 模式下同一连接同时承载 JPEG 和 PCM。

## 电源与输入

- PWR 短按在 Settings 中执行高亮项；按住约 0.7 秒返回（其他 App 仍是短按返回）。
- PWR 长按约 1.2 秒在任意界面弹出三按钮电源菜单：POWER OFF / REBOOT / CANCEL；默认高亮 CANCEL，LEFT/RIGHT 选择，PWR 短按执行，触摸可直接点按。
- PWR 持续按住 6 秒由 AXP2101 硬件断电，电源菜单不会阻止这个兜底路径。
- Settings 用 LEFT/RIGHT 移动高亮，按钮为 24 px 高并带填充/边框；触摸仍可直接点按。
- 主题只给前景着色并保留绿色点阵背景；ERROR 例外，会把暗绿色点阵/扫描线一起转成暗红色。
- PWR 短按/长按由 AXP2101 `PMIC_INT`（GPIO41）的正/负边沿中断提供；R107 已拆，GPIO0 不再作为 PWR fallback。
- AXP2101 `0x22` 使能 6 秒长按硬件关机。
- 空闲超时调用 `ft_pmic_poweroff()`，并把 `boot_fast=1` 写入 NVS；下次启动 UI 优先。
- 用户主动关机写入 `boot_fast=0`；低电压关机也按正常启动处理。
- 低电压且无 VBUS 时显示 3 秒警告后关机；插入 USB 会取消。
- RTC 无效时用编译时间播种；Wi-Fi 获得 IP 后启动 SNTP 并写回 PCF85063。
- 主题状态保存在 NVS：`theme` / `err_unlock`；ERROR 只能通过表盘日期 7 连点或 REPL 解锁。

## 脚本

`.wsh` / `.sh` 文件位于 `/storage/scripts`。终端行保存每个字符的颜色，
`watch_term_write_tagged()` 支持 `{red}...{/}`，`echo red "text"` 也可。
脚本执行有 120 s / 200000 次迭代上限，PWR 返回或 STOP 按钮会置停止标志。

GAMES 内的 `MATRIX` 不使用脚本：它直接按 40 列维护长字符串、头部亮色和绿色拖尾，全屏绘制；点屏幕回菜单，PWR 回转盘。

`HEX` 是二维正六边形运动学：6 个顶点直接按中心位置和角度做平面旋转，不再有 3D 姿态、12 条棱弹簧或 `SETTLED`。加速度计的屏幕平面重力驱动位置，陀螺仪 Z 轴驱动角度；撞到矩形场边界时使用 `0.55` 回弹，切向只做轻微阻尼，没有静摩擦吸附。晃动时把“原始加速度 - 低通重力”的高频分量作为惯性加速度。底部 `-` / `+` 调整 `0.1x–1.0x` 重力倍率（NVS 键 `hex_g`），点屏幕不重置。实机轴向为：竖直向下时 `ax≈-g`，逆时针 90° 后 `ay≈g`，所以重力取 `gx = -imu->ay`、`gy = -imu->ax`；物理固定 4 ms 子步。

`FLASK` 不是真实瓶形，而是一个和 HEX 同风格的密闭矩形容器。`watch_sensors_imu()` 的屏幕重力方向决定液面半平面，所以翻转 180° 后水会转到另一侧；高通加速度注入能量，49 点一维弹簧/耦合波方程生成水面，快速子步（5 ms）保留完整晃动。液面被裁剪在容器内，不会倒出；绘制时叠加多条水面等高线和波峰高光，没有气泡或点击重置。PWR 返回转盘。

## 已知限制

- 仅 GC9307 4 线 SPI；QSPI 无 DC 屏不支持。
- PICTURES 是静态 JPG/JPEG，不是 MJPEG 视频。
- VIDEO 只接受 WSV2；HTTP 拉流需要服务器先把 MP4 转成 JPEG+PCM，不支持本地视频文件解码。
- MCP 暂未实现；AI 只做 hello/ping/chat/listen/Opus/STT/LLM/TTS。AI 页的 `HELLO`/`PING` 预设用于分别测试文本链路和不依赖 ASR/AI 的 WS 往返。
- 低音量音频失真按硬件问题处理。
