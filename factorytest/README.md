# ESP_Watch 工厂测试（主工程组件）

代码集中在 `factorytest/` 文件夹，以**标准 ESP-IDF 组件**方式加入主工程
（根 `CMakeLists.txt` 的 `EXTRA_COMPONENT_DIRS`），随主工程一起用 `idf.py` 构建。
入口为 `factory_test_start()`，由 `main/main.c` 在 `CONFIG_FACTORY_TEST_MODE=y` 时调用。

## 1. 编译与烧录

在工程根目录执行：

```bash
cd /home/NaOH/Code/ESP32/ESP_Watch
source ~/.espressif/tools/activate_idf_v6.0.1.sh
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

- 工厂测试开关：`idf.py menuconfig → Factory test → Enable factory test firmware`（默认 n，WatchShell 默认启动）。
- `sdkconfig.defaults` 已按板载硬件设置：16 MB Flash、8 MB 八线 PSRAM、WatchShell 分区表、
  **主控制台 = USB-Serial-JTAG**（`/dev/ttyACMx`）。
- 若想改用 CH343P（`/dev/ttyUSBx`）交互：`menuconfig → Channel for console output`
  选择 UART0，REPL 会自动跟随主控制台（代码里按 `CONFIG_ESP_CONSOLE_*` 选择）。
- 关闭 `CONFIG_FACTORY_TEST_MODE` 后，`app_main` 启动 WatchShell；组件仍会编译，
  但工厂测试入口不会被调用。
- 不需要单独的工程/目录，也不需要自定义烧录脚本。

## 2. 启动时序（重要）

```
NVS → I2C0(GPIO9/10) → AXP2101 校验 ID=0x4A
    → DC1  3.3 V / ALDO3 3.3 V / DC3(VAUDIO) 3.3 V 使能
    → 延时 120 ms → LCD 初始化 → CST816D → ICM-42670 → 按键
    → 打印 banner → 进入 REPL
```

`DC3 = VAUDIO` 默认是关闭的，必须由本固件打开，否则 MAX98357A 无声；
`ALDO3 = AVCC` 未开时所有传感器/麦克风都不响应。

## 3. 快速使用流程（建议顺序）

上电后进入 `factorytest>`，按下面顺序逐项确认：

```
info
selftest
lcd bars
lcd grad
lcd grid
lcd bright 100
lcd bright 10
lcd bright 70
touch stream 200
audio tone 3
audio mic 5
audio echo 10 8
imu stream 10
aht read
rtc set
rtc read
rtc tick
keys stream 10
pmic adc
flash
usb
wifi scan 8
bt scan 5
```

### 3.1 `info` 输出怎么看

以实测输出为例：

- `battery: absent` + `VBAT=1708 mV (floating, no battery)`：
  没接电池时 VBAT 是浮空值，**正常**；接电池后应显示 present 且电压随电量变化。
- `VBUS: present` + `VBUS≈5000 mV`：USB 供电正常；`VSYS≈5.1 V` 也正常。
- `VCC/AVCC/VAUDIO` 三路应为 **ON / 3300 mV**；VAUDIO 是功放电源，缺失会导致无声。
- `PCF85063 time=2000-01-01 ...`：RTC 未设置时上电默认值，但秒在走；
  执行 `rtc set` 后变为 2026-11-18 12:00:00。
- `keys` 行反映**当前**电平：按住左键时 `LEFT=DOWN`，松开恢复 `up`。
- `reset reason` 现在能显示 `jtag` / `usb` / `power-on` 等。
- `PSRAM size=8388608`、`flash id=0xEF4018 size=16 MB` 与板载一致。

### 3.2 屏幕

```
lcd info                  # ctrl=GC9307 240x284
lcd res 240 284 0 0       # 默认；偏移不对时再调
lcd bars                 # 红/绿/蓝/青/紫/黄 六条
lcd grad                 # RGB 渐变
lcd grid                 # 网格 + 型号/分辨率文字
lcd text HELLO           # 文字
lcd pattern              # 黑白棋盘
lcd bright 0..100        # 背光调光
lcd off / lcd on
lcd rot 0..3             # 只改 MADCTL
lcd mode 0|3             # SPI 模式（黑屏时可试 3）
lcd speed 10|20|40       # SPI 时钟 MHz（长线/六线屏可降速）
```

- 面板没有 MISO，**只能目视确认**，无法读回。
- 白屏/花屏/黑屏：先试 `lcd mode 3`、`lcd speed 10`、`lcd res 240 284 0 0`；
  仍不对则检查排线、DC/RST、以及六线屏 CS 是否接地。
- 全黑但有背光：检查 `LCD_DC/CS/RST` 与 CN1 排线；`lcd bright 100` 确认背光。
- 六线屏（VCC/GND/SCL/SDA/RES/DC，无 CS）把模块 CS 接地即可，CS 由驱动翻转不影响。
- 无 DC 的 QSPI 屏本硬件无法驱动（板子只引出 4 线 SPI 的 DC）。

### 3.3 触摸

```
touch id            # 应显示 id=0xB6 fw=0x02 INT=high
touch read          # 单次坐标/手势
touch trace 10      # 10 秒连续画线，推荐用这个看跟手性
touch stream 200    # 每次采样画一个十字，看到的是红色区域在动
touch int           # 观察 INT 线变化 5 秒
```

`touch trace` 会把相邻采样点连成线；`touch stream` 只在每个采样点画 9×9 十字，
所以手指移动快时会看到一片红色区域，而不是一条线，这是测试命令的画法差异。

### 3.4 音频

```
pmic rail           # 确认 VAUDIO(DC3) = ON 3300 mV
audio tone 3        # 1 kHz 单音：只验证功放+喇叭
audio mic 5         # 只读麦克风：说话/轻敲麦克风，看 rms/peak 变化
audio echo 10 8     # 麦克风回显到喇叭，增益 0.8
```

- **先 `audio tone` 再 `audio mic`**，可快速区分"喇叭不响"还是"麦克风没信号"。
- 回显容易啸叫：默认增益 0.8，最大 2.0；测试时降低音量或遮挡麦克风。
- 完全无声：`pmic rail` 看 VAUDIO；再 `pmic on vaudio`。

### 3.5 传感器 / RTC / 按键

```
imu id                 # WHO_AM_I=0x67
imu stream 10          # 加速度/陀螺/温度，每 100 ms 一行
mag id                 # QMC5883P CHIP_ID=0x80
aht read               # 温度/湿度
rtc set / rtc read     # 设置并读回 2026-11-18 12:00:00
rtc tick               # 1.2 s 内秒计数应 +1
rtc int                # 定时器中断，GPIO42 应变低
keys / keys stream 10  # 电源/左/右三键
```

### 3.7 Wi-Fi / BLE

```
wifi scan 10                 # 扫描 2.4 GHz AP，列出 RSSI/信道/加密/SSID/BSSID
wifi connect <ssid> [pass]   # 连接并等 DHCP，拿到 IP 后自动断开
wifi status                  # STA MAC / 当前连接 / IP
bt scan 5                    # BLE 扫描 5 秒，打印地址/RSSI/广播名
```

- ESP32-S3 **只有 BLE，没有经典蓝牙**；`bt scan` 用 NimBLE Observer。
- `wifi connect` 只用于工厂验证，成功拿到 IP 后会主动断开，不会保持联网。
- Wi-Fi 首次初始化约 1 秒；BLE 每次扫描都会 init/deinit NimBLE，便于反复测试。
- Wi-Fi 与 BLE 共用射频，已启用软件共存；扫描时不要同时跑大流量音频。
- 只支持 2.4 GHz；5 GHz AP 不会出现在 `wifi scan` 中。

### 3.8 电源与总线

```
pmic info | rail | adc
pmic on|off vaudio
pmic set avcc 3300
i2c scan sensor      # 期望 0x34 0x2C 0x38 0x51
i2c scan touch       # 期望 0x15
flash
usb
```


## 4. 命令

| 命令 | 说明 |
|------|------|
| `help` | 列出命令 |
| `info` | 芯片/Flash/PSRAM/MAC/PMIC/屏幕/触摸/传感器概览 |
| `selftest` | 一键自检（不发声、不含目视项） |
| `i2c scan [sensor\|touch\|all]` | 扫描两路 I2C |
| `pmic info` / `rail` | PMIC 状态与三路电源 |
| `pmic init` | 重新执行上电配置 |
| `pmic on\|off <vcc\|avcc\|vaudio>` | **VAUDIO/AVCC 开关** |
| `pmic set <vcc\|avcc\|vaudio> <mV>` | 设置电压（带步进校验） |
| `pmic adc` | VBAT / VSYS / VBUS / 电量 |
| `pmic poweroff` | AXP2101 关机 |
| `imu id\|read\|stream [n]` | ICM-42670（SPI） |
| `mag id` | QMC5883P（I2C 0x2C，ID=0x80） |
| `aht read` | AHT20（I2C 0x38） |
| `rtc read\|set\|tick\|int` | PCF85063（I2C 0x51） |
| `lcd init\|info\|res\|rot\|mode\|speed\|bright\|on\|off` | 屏幕控制 |
| `lcd fill <color>` | 纯色（支持 red/green/blue/... 或 0xRRGGBB） |
| `lcd bars\|grad\|grid\|pattern\|text <str>` | 显示图案 |
| `touch id\|read\|stream [n]\|trace [sec]\|int` | CST816D；`trace` 连续画线，`stream` 画十字 |
| `keys [stream [sec]]` | 电源/左/右三键 |
| `audio mic [sec]` | 只读麦克风，打印 RMS/峰值 |
| `audio tone [sec]` | 1 kHz 单音，只测功放/喇叭 |
| `audio echo [sec] [gain]` | 麦克风 → 喇叭回显，gain 单位 0.1 |
| `wifi scan [n]` / `connect` / `status` | 2.4 GHz 扫描、STA 连接/DHCP、状态 |
| `bt scan [sec]` | BLE 扫描（NimBLE） |
| `flash` / `usb` | Flash ID、USB-JTAG 状态 |
| `reboot` | 重启 |

示例：

```
pmic on vaudio          # 打开功放电源
lcd bright 100
lcd bars
touch trace 10          # 连续画线，确认跟手性
audio mic 5             # 先确认麦克风有信号
audio tone 3            # 再确认喇叭响
audio echo 10 6         # 最后低增益回显
wifi scan 8             # 确认 2.4 GHz 射频
bt scan 5               # 确认 BLE 射频
```

## 5. 自检判据

| 项目 | 判据 |
|------|------|
| PMIC | ID=0x4A，DC1/ALDO3/DC3 均处于开启 |
| 传感器 I2C | 0x34 / 0x2C / 0x38 / 0x51 均应答 |
| IMU | `WHO_AM_I(0x75)` = 0x67 |
| 磁力计 | `CHIP_ID(0x00)` = 0x80 |
| 温湿度 | 状态寄存器可写/可读，测量值在合理范围 |
| RTC | 控制寄存器可读，秒计数在 1.2 s 内递增 |
| 触摸 | Chip ID（0xA7）= 0xB6（CST816D） |
| Flash | JEDEC ID = 0xEF4018（W25Q128，16 MB） |
| 按键 | 三键均可读（默认松开为高） |
| 屏幕 | 需人工目视确认（面板无 MISO，无法读回） |
| Wi-Fi | `wifi scan` 至少列出 1 个 2.4 GHz AP；`wifi connect` 能拿到 IP |
| BLE | `bt scan 5` 至少收到 1 个广播（手机/耳机/手环） |

## 6. 屏幕与触摸参数

- 固定 **GC9307 240×284**；初始化参数可在 `menuconfig → Factory test` 修改。
- RGB565 按大端发送：驱动写帧缓冲时已交换高低字节，颜色无需再调 BGR。
- 运行时：
  ```
  lcd res 240 284 [xoff] [yoff]
  lcd mode 0|3
  lcd speed 10|20|40
  ```
- 4 线 SPI：CLK=GPIO17、MOSI=GPIO16、CS=GPIO18、DC=GPIO11、RST=GPIO38；
  背光 LEDC=GPIO39（高电平点亮）；TE=GPIO21。
- 触摸 CST816D：I2C 0x15，ID 0xA7=0xB6，FW 0xA9；
  RST=GPIO14，INT=GPIO15；坐标寄存器 0x03..0x06。
- 帧缓冲 240×284×2 ≈ 136 KB；优先内部 RAM，分配失败时用 PSRAM，再失败只剩纯色填充。

## 7. 音频说明

- I2S 全双工：BCLK=GPIO6、WS=GPIO8、DOUT=GPIO5、DIN=GPIO7，16 kHz/16 bit/立体声。
- 麦克风 L/R 接地 → 左声道；输出左右声道写同一采样，避免 MAX98357A
  `SD_MODE` 声道模式不确定。
- 依赖 `VAUDIO`(DC3) 与 `AVCC`(ALDO3)；启动时已自动打开。
- **回声测试容易啸叫**：先 `audio mic` 与 `audio tone` 分开验证，
  回声默认增益 0.8，上限 2.0。

## 8. 常见问题

| 现象 | 排查 |
|------|------|
| `pmic` 显示 not found | AXP2101 地址 0x34，检查 I2C0 (GPIO9/10) 与 VCC |
| 所有传感器不应答 | `pmic rail` 看 `AVCC(ALDO3)` 是否为 ON，必要时 `pmic on avcc` |
| 喇叭无声 | `pmic rail` 看 `VAUDIO(DC3)` 是否 ON；再 `audio tone` |
| 屏幕白屏/花屏/黑屏 | 先 `lcd mode 3`、`lcd speed 10`、`lcd res 240 284 0 0`；检查 DC/RST/CS；QSPI 无 DC 的屏本硬件不支持 |
| 触摸无坐标 | `touch id` 是否 0xB6；检查 CN1 pin17..20 |
| 触摸像红色区域而不是线 | 正常：`touch stream` 画十字；用 `touch trace 10` 看连续线 |
| `wifi scan` 无结果 | 确认附近有 2.4 GHz AP、天线已接；5 GHz 不支持 |
| `bt scan` 无结果 | 确认附近有 BLE 设备；ESP32-S3 无经典蓝牙 |
| AHT20 read failed | 已改为纯字节命令（0xAC 0x33 0x00）并重试；若仍失败检查 0x38 地址与 AVCC |
| 串口无输出 / 交互超时 | 主控制台是 USB-Serial-JTAG，用 `/dev/ttyACMx`；若用 CH343P，请在 menuconfig 把主控制台改为 UART0 |

## 9. 已知限制

- 面板无 MISO，无法自动识别控制器/分辨率，只能目视确认。
- `lcd rot` 只改 MADCTL，不交换宽高。
- 没有做 USB CDC 数据回环；USB 只通过 USB-Serial-JTAG 枚举状态判断。
- BLE 只做扫描（Observer），未做连接/配对/吞吐测试。
- Wi-Fi 只做扫描与 STA 连接/DHCP，未做 RF 指标或吞吐测试。
- Wi-Fi 与 BLE 共用射频，重载音频/显示时吞吐会下降。

## 10. 下载模式 / 电源键 = BOOT（重要）

本板 `GPIO0` 经 R107(0 Ω) 与 `PWRON` 相连，也就是说 **电源键同时是 BOOT 键**。
如果复位/上电瞬间按键仍被按住，ROM 会进入下载模式：

```
boot:0x0 (DOWNLOAD(USB/UART0))
waiting for download
```

这不是固件问题，也不是烧录失败。处理方法：

1. **松开电源键**，再让芯片复位一次（按键不能按着）：
   - `idf_monitor` 里按 `Ctrl+T` 然后 `Ctrl+R`（经 USB-Serial-JTAG 复位）；
   - 或执行 `idf.py -p /dev/ttyACM2 flash`，结束时的 hard-reset 会自动重启；
   - 或无电池时拔插 USB（按键保持松开）。
2. 正常启动的日志应类似 `boot:0x8 (SPI_FAST_FLASH_BOOT)`，随后才能看到 factorytest banner。
3. 若松开按键仍然进下载模式：
   - 检查 SW3 pin4 / `PWRON` 是否对地短路、按键是否卡住；
   - 复位瞬间测 `GPIO0` 应约为 3.3 V；
   - 若为了使用 `PMIC_INT` 拆掉了 R107，必须在 GPIO0 与 VCC 之间补 10 kΩ 上拉，否则 GPIO0 会失去 PWRON/R15 的上拉路径；
   - AXP2101 冷启动时 `PWRON` 可能短暂为低，可先上电、松键、等 1 s，再用 `Ctrl+T Ctrl+R` 复位。

> 另外：现在工厂测试已并入主工程，VSCode 扩展监视的
> `/home/NaOH/Code/ESP32/ESP_Watch/build/ESP_Watch.elf` 就包含 factorytest，
> 直接在工程根目录 `idf.py build / flash monitor` 即可，不需要切换工程。
