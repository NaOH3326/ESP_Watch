# ESP_Watch 引脚-外设映射表

> **数据来源**：`Doc/Netlist_Schematic1_2026-11-18.enet`（嘉立创 EDA 导出的 JSON 网表）
> **主控**：ESP32-S3R8，QFN-56 封装（网表位号 **U2**，57 个焊盘 = 56 pin + 散热焊盘 EP）
> **配套头文件**：`main/board_pins.h`
> **生成日期**：2026-11-18

---

## 0. 阅读须知

1. **网表中的引脚号是 QFN 封装引脚号，不是 GPIO 编号。**
   例如背光控制信号在 U2 的 **pin44**，但它是 **GPIO39**；写代码时要用 GPIO39。
2. 头文件里的宏名 = `GPIO_` + 网表网络名，方便与原理图互相反查：
   网络 `LCD_BLK` → `GPIO_LCD_BLK`；网络 `I2S_SD` → `GPIO_I2S_SD`。
3. 下表的“方向”以 **ESP32-S3 为参考**（输出 = ESP32 驱动对方）。
4. 本表只保留与 GPIO / 电源 / 连接器相关的连接关系，网表中电容、电感、电阻的
   采购属性等冗余信息已全部过滤。
5. 网表中以 `$1Nxx` 命名的自动网络号，本文档都注明了它的实际作用。

---

## 1. ESP32-S3 已使用 GPIO 总表

| GPIO | U2 引脚 | 网络名 | 外设 / 信号 | 方向 | 备注 |
|-----:|--------:|--------|-------------|:----:|------|
| 0 | 5 | `GPIO0` | BOOT / 启动 strapping | 输入 | **R107 已拆，GPIO0 不再接 PWRON**；必须外接 10 kΩ 上拉到 VCC；**strapping** |
| 1 | 6 | `IMU_MISO` | U3 ICM-42670-P `AP_SDO/AP_AD0` | 输入 | 传感器 SPI MISO |
| 2 | 7 | `IMU_MOSI` | U3 `AP_SDA/AP_SDIO/AP_SDI` | 输出 | 传感器 SPI MOSI |
| 3 | 8 | `IMU_SCK` | U3 `AP_SCL/AP_SCLK` | 输出 | 传感器 SPI CLK |
| 4 | 9 | `IMU_CS` | U3 `AP_CS` | 输出 | 传感器 SPI CS |
| 5 | 10 | `I2S_DIN` | U10 MAX98357A `DIN` | 输出 | I2S 音频数据 → 功放 |
| 6 | 11 | `I2S_SCK` | U10 `BCLK` + U9 麦克风 `SCK` | 输出 | 功放与麦克风共用位时钟 |
| 7 | 12 | `I2S_SD` | U9 MSM261S4030H0R `SD` | 输入 | 麦克风音频数据，R13 100 kΩ 下拉 |
| 8 | 13 | `I2S_WS` | U10 `LRCLK` + U9 `WS` | 输出 | 左右声道 / 帧同步 |
| 9 | 14 | `IIC_SCL` | U4/U6/U11/U14 的 SCL | 输出(OD) | R8 4.7 kΩ 上拉到 VCC |
| 10 | 15 | `IIC_SDA` | U4/U6/U11/U14 的 SDA | 双向(OD) | R9 4.7 kΩ 上拉到 VCC |
| 11 | 16 | `LCD_DC` | CN1 pin24 | 输出 | 显示 SPI 数据/命令选择 |
| 12 | 17 | `LCD_TOUCH_SCL` | CN1 pin20 | 输出(OD) | 触摸 I2C，R95 4.7 kΩ 上拉 |
| 13 | 18 | `LCD_TOUCH_SDA` | CN1 pin19 | 双向(OD) | 触摸 I2C，R94 4.7 kΩ 上拉 |
| 14 | 19 | `LCD_TOUCH_RST` | CN1 pin18 | 输出 | 触摸复位 |
| 15 | 21 | `LCD_TOUCH_INT` | CN1 pin17 | 输入 | 触摸中断；`XTAL_32K_P`，RTC GPIO，可深睡唤醒 |
| 16 | 22 | `LCD_SDA` | CN1 pin7 | 输出 | **显示 SPI MOSI**（不是 I2C！）；`XTAL_32K_N` |
| 17 | 23 | `LCD_SCL` | CN1 pin5 | 输出 | 显示 SPI CLK |
| 18 | 24 | `LCD_CS` | CN1 pin4 | 输出 | 显示 SPI CS |
| 19 | 25 | `ESP_DM` | U8 CH334P `DM2` | 双向 | USB D-（USB-JTAG / OTG） |
| 20 | 26 | `ESP_DP` | U8 CH334P `DP2` | 双向 | USB D+（USB-JTAG / OTG） |
| 21 | 27 | `LCD_TE` | CN1 pin3 | 输入 | 显示撕裂同步（Tearing Effect），RTC GPIO |
| 27 | 30 | `FLASH_SPI_HD` | U5 W25Q128 `HOLD#/RESET#(IO3)` | — | SPI0/1 专用 |
| 28 | 31 | `FLASH_SPI_WP` | U5 `WP#(IO2)` | — | SPI0/1 专用 |
| 29 | 32 | `FLASH_SPI_CS0` | U5 `CS#` | — | SPI0/1 专用 |
| 30 | 33 | `FLASH_SPI_CLK` | U5 `CLK` | — | SPI0/1 专用 |
| 31 | 34 | `FLASH_SPI_Q` | U5 `DO(IO1)` | — | SPI0/1 专用 |
| 32 | 35 | `FLASH_SPI_D` | U5 `DI(IO0)` | — | SPI0/1 专用 |
| 38 | 43 | `LCD_RST` | CN1 pin1 | 输出 | 显示复位 |
| 39 | 44 | `LCD_BLK` | U13 2N7002K 栅极 | 输出 | **高电平 = 背光开**；低边开关，可用 LEDC PWM |
| 40 | 45 | `KEY_RIGHT` | SW3 pin2 | 输入 | 无外部上拉，**必须开内部上拉**（C18 100 nF 滤波） |
| 41 | 47 | `PMIC_INT` | U14 AXP2101 `IRQ` | 输入 | R6 10 kΩ 需改接常电域（原 VCC 错误） |
| 42 | 48 | `RTC_INT` | U11 PCF85063 `INT#` | 输入 | R4 10 kΩ 上拉，低电平有效 |
| 43 | 49 | `ESP32_TX` | U1 CH343P `RXD` | 输出 | UART0 TX |
| 44 | 50 | `ESP32_RX` | U1 CH343P `TXD` | 输入 | UART0 RX |
| 45 | 51 | `KEY_LEFT` | SW3 pin1 | 输入 | 无外部上拉，**必须开内部上拉**；**strapping** |
| 46 | 52 | `$1N30` | 启动模式配置 | 输入 | R10 100 kΩ 下拉，保证 SPI 启动；**不要作他用** |

---

## 2. 未使用 / 保留的 GPIO

| GPIO | U2 引脚 | 芯片内名称 | 状态 | 说明 |
|-----:|--------:|-----------|------|------|
| 22 ~ 25 | — | — | 不存在 | ESP32-S3 只有 GPIO0~21、GPIO26~48，共 45 个 |
| 26 | 28 | `SPICS1` | NC | 本设计未连接；可作普通 GPIO（未布线） |
| 33 ~ 37 | 38 ~ 42 | `GPIO33`~`GPIO37` | 禁止使用 | ESP32-S3R8 片内**八线 PSRAM** 占用（SPIIO4~7、SPIDQS） |
| 47 | 36 | `SPICLK_N` | NC | 未连接，建议保留 |
| 48 | 37 | `SPICLK_P` | NC | 未连接，建议保留 |

---

## 3. 非 GPIO 引脚（电源 / 晶振 / 射频）

| U2 引脚 | 名称 | 连接网络 | 说明 |
|--------:|------|----------|------|
| 1 | `LNA_IN` | `$1N349` | 射频输入：L2 1.3 nH 并联到地 → R7 0 Ω → 陶瓷天线 U12（$1N348 处 C3 1 pF 到地） |
| 2, 3 | `VDD3P3` | `$1N355` | 经 L3 2.0 nH 由 VCC 供电 |
| 4 | `CHIP_PU` | `PMIC_PG` | 由 AXP2101 `PWROK`(pin29) 驱动，R2 10 kΩ 上拉；高电平释放复位 |
| 20 | `VDD3P3_RTC` | `VCC` | 3.3 V |
| 29 | `VDD_SPI` | `VCC` | Flash 供电 3.3 V |
| 46 | `VDD3P3_CPU` | `VCC` | 3.3 V |
| 53 | `XTAL_N` | `XTAL_N` | U15 40 MHz 晶振，C4 10 pF |
| 54 | `XTAL_P` | `XTAL_P` | U15 40 MHz 晶振，C5 10 pF |
| 55, 56 | `VDDA` | `VCC` | 模拟电源 3.3 V |
| 57 | `GND` / EP | `GND` | 散热焊盘接地 |

---

## 4. 按外设分组

### 4.1 六轴 IMU —— ICM-42670-P（U3，SPI 模式）

| 信号 | ESP32 GPIO | U2 引脚 | U3 引脚 |
|------|-----------:|--------:|--------:|
| `IMU_SCK` | 3 | 8 | 13 `AP_SCL/AP_SCLK` |
| `IMU_MOSI` | 2 | 7 | 14 `AP_SDA/AP_SDIO/AP_SDI` |
| `IMU_MISO` | 1 | 6 | 1 `AP_SDO/AP_AD0` |
| `IMU_CS` | 4 | 9 | 12 `AP_CS` |

- U3 供电 `AVCC`（VDDIO pin5、VDD pin8），`FSYNC` 接地，`INT1/INT2` 未连接。
- 该器件支持 I3C / I2C / SPI，本设计使用 SPI（I2C 地址选择脚 `AP_SDO/AP_AD0` 用作 MISO）。

### 4.2 传感器 I2C 总线（`IIC_SCL` = GPIO9，`IIC_SDA` = GPIO10）

一条总线挂 4 个从机，R8 / R9（4.7 kΩ）上拉到 `VCC`：

| 位号 | 型号 | 功能 | SCL 引脚 | SDA 引脚 |
|------|------|------|---------|---------|
| U4 | QMC5883P | 三轴磁力计 | 1 `SCK` | 16 `SDA` |
| U6 | AHT20 | 温湿度传感器 | 3 `SCL` | 4 `SDA` |
| U11 | PCF85063ATL | RTC（外接 X1 32.768 kHz） | 7 `SCL` | 6 `SDA` |
| U14 | AXP2101 | 电源管理 PMIC | 40 `SCK` | 39 `SDA` |

- U4 的 `C1`(pin10) 外接 C30 1 µF。
- 各器件 I2C 地址请以各自数据手册为准。

### 4.3 显示屏（SPI，经 CN1 24P FPC）

| 信号 | ESP32 GPIO | U2 引脚 | CN1 引脚 | 说明 |
|------|-----------:|--------:|---------:|------|
| `LCD_SCL` | 17 | 23 | 5 | SPI CLK |
| `LCD_SDA` | 16 | 22 | 7 | SPI MOSI（网络名叫 SDA） |
| `LCD_CS` | 18 | 24 | 4 | 片选 |
| `LCD_DC` | 11 | 16 | 24 | 数据/命令 |
| `LCD_RST` | 38 | 43 | 1 | 复位 |
| `LCD_TE` | 21 | 27 | 3 | 撕裂同步输入 |
| `LCD_BLK` | 39 | 44 | 13、14 | 背光使能（见下） |

背光电路：`LCD_BLK` → U13（2N7002K）栅极，源极接地，漏极 → R1 10 Ω → CN1 pin13/14。
因此 **GPIO39 输出高电平点亮背光**，可用 LEDC 做 PWM 调光。

面板控制器（二选一，需按实际屏确认）：

| 控制器 | 分辨率 | 初始化表来源 | 备注 |
|--------|--------|--------------|------|
| GC9307 | 240×284 | gc9307-async-rs 厂商序列 | RGB565，MADCTL=0x48 |

- 面板只有 MOSI、没有 MISO，**无法读 ID**，只能靠目视/批次记录确认控制器。
- 4 线 SPI，`esp_lcd_panel_io_spi`（8 bit cmd + 8 bit param），建议 40 MHz。
- 偏移/分辨率默认 `240 284 0 0`，异常时用 `lcd res 240 284 <xoff> <yoff>` 调整。
- RGB565 数据按大端发送，驱动写帧缓冲时已交换高低字节；颜色异常先查这里，而不是 MADCTL 的 BGR 位。
- 六线 SPI 屏（VCC/GND/SCL/SDA/RES/DC，无 CS）也能用：把模块 CS 接地即可，
  本驱动仍会正常翻转 CS（模块不接 CS 时无影响）。
- **无 DC 的 QSPI 屏本硬件无法驱动**（板子只引出 4 线 SPI 的 DC）。
- 帧缓冲可放 PSRAM，触摸轨迹叠在帧缓冲上再刷屏。

### 4.4 触摸屏（I2C，经 CN1）

| 信号 | ESP32 GPIO | U2 引脚 | CN1 引脚 | 备注 |
|------|-----------:|--------:|---------:|------|
| `LCD_TOUCH_SCL` | 12 | 17 | 20 | R95 4.7 kΩ 上拉 |
| `LCD_TOUCH_SDA` | 13 | 18 | 19 | R94 4.7 kΩ 上拉 |
| `LCD_TOUCH_RST` | 14 | 19 | 18 | 复位 |
| `LCD_TOUCH_INT` | 15 | 21 | 17 | 中断输入，RTC GPIO（可深睡唤醒） |

触摸 I2C 与传感器 I2C 是两条**独立**总线，不能混用同一组 GPIO。

触摸控制器为 **CST816D**（7 位地址 **0x15**）：

| 寄存器 | 含义 | 说明 |
|--------|------|------|
| `0xA7` | Chip ID | CST816D 应为 **0xB6**（CST816S/T 为 0xB4/0xB5） |
| `0xA9` | 固件版本 | 只读 |
| `0x00` | 状态 | bit7 为忙标志 |
| `0x01` | 手势 ID | 滑动/点击等 |
| `0x02` | 触点数 | 低 4 位 |
| `0x03/0x04` | X 高/低 | 12 bit：`((高&0x0F)<<8) | 低` |
| `0x05/0x06` | Y 高/低 | 同上 |
| `0xFE` | 关闭自动休眠 | 写 1 可在测试期间保持唤醒 |
| `0xE5` | 进入休眠 | 写 3 |

- 复位：GPIO14 低 30 ms → 高 60 ms；中断：GPIO15，无外部上拉。
- 触摸面板的 VCC 来自 `VCC`（屏排线 pin11/12/15/16），与 AXP2101 DC1 同步。

### 4.5 音频（I2S，功放 + 麦克风共用时钟）

| 信号 | ESP32 GPIO | U2 引脚 | 功放 U10 MAX98357A | 麦克风 U9 MSM261S4030H0R |
|------|-----------:|--------:|-------------------|--------------------------|
| `I2S_SCK` | 6 | 11 | pin16 `BCLK` | pin6 `SCK` |
| `I2S_WS` | 8 | 13 | pin14 `LRCLK` | pin3 `WS` |
| `I2S_DIN` | 5 | 10 | pin1 `DIN` | — |
| `I2S_SD` | 7 | 12 | — | pin7 `SD` |

- 功放：`GAIN_SLOT` 经 R26 100 kΩ 下拉，`SD_MODE#` 经 R3 680 kΩ 上拉；
  `OUTP/OUTN` 经 L5/L4 2.2 µH 与 C14/C7 220 pF 送到 `SPEAKER_P`/`SPEAKER_N`（TP2/TP1）。
- 麦克风：`CHIPEN` 经 R11 10 kΩ 上拉（常开），`L/R` 经 R14 10 kΩ 下拉（左声道），
  `SD` 经 R13 100 kΩ 下拉；`VDD` 接 `AVCC`。
- U10 供电为独立音频域 `VAUDIO`。

### 4.6 USB 通路

```
Type-C(USB1) ──D+/D-──► CH334P(U8) 上行口 DMU/DPU
                         ├─ 下行口1 DM1/DP1 ──► CH343P(U1) ──UART──► ESP32 GPIO43/44
                         └─ 下行口2 DM2/DP2 ──► ESP32 GPIO19/GPIO20（原生 USB）
```

- ESP32 侧：`ESP_DM` = GPIO19，`ESP_DP` = GPIO20（默认即 USB-JTAG 引脚）。
- Type-C 的 CC1/CC2 各经 5.1 kΩ（R12/R5）下拉；D5 为 VBUS ESD 保护。
- CH334P 的 `VDD33`(pin16) 是其内部 3.3 V LDO 输出，只接 C35/C51 去耦；
  `V5`(pin15) 接 `VBUS`，晶振为 X4 12 MHz。
- CH343P 的 `V3`(pin6) 为内部 LDO 输出，接 C34/C50；`VIO`(pin1) 接 `VCC`。

### 4.7 调试串口 UART0

| ESP32 信号 | GPIO | U2 引脚 | CH343P(U1) 引脚 |
|-----------|-----:|--------:|----------------:|
| `ESP32_TX`（`U0TXD`） | 43 | 49 | pin5 `RXD` |
| `ESP32_RX`（`U0RXD`） | 44 | 50 | pin4 `TXD` |

这是 ESP32-S3 的默认下载/日志串口。

### 4.8 按键

| 网络 | ESP32 GPIO | U2 引脚 | SW3 引脚 | 说明 |
|------|-----------:|--------:|---------:|------|
| `KEY_LEFT` | 45 | 51 | 1 | 按下接地；C1 100 nF；**无外部上拉**，需内部上拉 |
| `KEY_RIGHT` | 40 | 45 | 2 | 按下接地；C18 100 nF；**无外部上拉**，需内部上拉 |
| `PWRON` | —（R107 已拆） | — | 4 | 电源键，接 AXP2101 `PWRON`(pin30)；内部 100 kΩ 上拉，R15 外部上拉可去掉 |

- SW3 为 TM-2024A 导航/侧键开关，本设计只用了 3 个触点，pin3/5/6/7/8 全部接地。
- **R107 已拆**：GPIO0 与 PWRON 断开，电源键不再兼作 BOOT 键；下载改用 USB-Serial-JTAG 或测试点。
- GPIO0 现在只做启动 strapping，**必须在 GPIO0 与 VCC 之间补 10 kΩ 上拉**；否则浮空可能进入下载模式。
- WatchShell 运行时以 `PMIC_INT`(GPIO41) 的 PWRON 边沿中断为主；GPIO0 不再作为 PWR fallback。
- 若屏幕不亮且串口为 `boot:0x0 DOWNLOAD`，先检查 GPIO0 的 10 kΩ 上拉，再测 PWRON 静态电压和按键是否卡住。
- AXP2101 已配置 PWRON 长按 6 秒硬件关机；长按约 1.2 秒由 UI 弹出三按钮电源菜单。

### 4.9 中断信号汇总

| 信号 | GPIO | 方向 | 有效电平 / 说明 |
|------|-----:|------|----------------|
| `PMIC_INT` | 41 | 输入 | AXP2101 `IRQ`，R6 10 kΩ 需接常电域（原接 VCC） |
| `RTC_INT` | 42 | 输入 | PCF85063 `INT#`，低有效，R4 10 kΩ 上拉 |
| `LCD_TOUCH_INT` | 15 | 输入 | 触摸中断（极性由触摸芯片配置决定） |
| `LCD_TE` | 21 | 输入 | 显示 TE 同步信号（可接 LCD 外设或做帧同步中断） |
| `GPIO0` / `KEY_LEFT` / `KEY_RIGHT` | 0 / 45 / 40 | 输入 | 按键中断（下降沿） |

### 4.10 外部 Flash

| 信号 | GPIO | U2 引脚 | U5 W25Q128JVPIQ |
|------|-----:|--------:|-----------------|
| `FLASH_SPI_CS0` | 29 | 32 | pin1 `CS#` |
| `FLASH_SPI_Q` | 31 | 34 | pin2 `DO(IO1)` |
| `FLASH_SPI_WP` | 28 | 31 | pin3 `WP#(IO2)` |
| `FLASH_SPI_D` | 32 | 35 | pin5 `DI(IO0)` |
| `FLASH_SPI_CLK` | 30 | 33 | pin6 `CLK` |
| `FLASH_SPI_HD` | 27 | 30 | pin7 `HOLD#/RESET#(IO3)` |

Flash 容量 128 Mbit（16 MB），由 `VDD_SPI`（= VCC 3.3 V）供电。
这 6 个引脚属于 SPI0/1 专用总线，正常应用不要复用。


---

## 5. CN1 24P FPC 连接器（显示 + 触摸）

| CN1 引脚 | 网络 | 去向 | 说明 |
|---------:|------|------|------|
| 1 | `LCD_RST` | GPIO38 | 屏复位 |
| 2 | `GND` | — | |
| 3 | `LCD_TE` | GPIO21 | 撕裂同步 |
| 4 | `LCD_CS` | GPIO18 | 屏片选 |
| 5 | `LCD_SCL` | GPIO17 | SPI CLK |
| 6 | NC | — | 未连接 |
| 7 | `LCD_SDA` | GPIO16 | SPI MOSI |
| 8 | `GND` | — | |
| 9 | NC | — | 未连接 |
| 10 | NC | — | 未连接 |
| 11 | `VCC` | 3.3 V | 屏电源 |
| 12 | `VCC` | 3.3 V | 屏电源 |
| 13 | `$1N79` | 背光 LED- | 经 R1 10 Ω 接 Q13 漏极 |
| 14 | `$1N79` | 背光 LED- | 同上 |
| 15 | `VCC` | 3.3 V | 屏电源 |
| 16 | `VCC` | 3.3 V | 屏电源 |
| 17 | `LCD_TOUCH_INT` | GPIO15 | 触摸中断 |
| 18 | `LCD_TOUCH_RST` | GPIO14 | 触摸复位 |
| 19 | `LCD_TOUCH_SDA` | GPIO13 | 触摸 I2C SDA |
| 20 | `LCD_TOUCH_SCL` | GPIO12 | 触摸 I2C SCL |
| 21 | `GND` | — | |
| 22 | NC | — | 未连接 |
| 23 | NC | — | 未连接 |
| 24 | `LCD_DC` | GPIO11 | 数据/命令 |
| 25 ~ 28 | `GND` | — | 固定/屏蔽地 |

---

## 6. 电源域（AXP2101，U14）

| 网络 | 产生方式 | 主要负载 |
|------|----------|----------|
| `VBUS` | Type-C 5 V 输入 | AXP2101 `VBUS`、CH334P `V5`、CH343P `VDD5/VBUS`、D5 ESD |
| `VBAT` | 电池（TP4 为电池电压测试点） | AXP2101 `BAT` |
| `VSYS` | AXP2101 系统输出 | `VIN1/VIN3/ALDOIN/VMID`；R19 10 kΩ → LED1 阳极 |
| `VCC` | AXP2101 DC1（`LX1` + L1 2.2 µH + `FB1`），3.3 V | ESP32 全部数字/模拟电源、W25Q128、CH343P `VIO`、两路 I2C 上拉 |
| `AVCC` | AXP2101 ALDO3（pin16），电压由寄存器配置 | ICM-42670-P、QMC5883P、AHT20、麦克风 U9 |
| `VAUDIO` | AXP2101 DC3（`LX3` + L6 2.2 µH + `FB3`），电压由寄存器配置 | MAX98357A `VDD` |
| `VRTC` | AXP2101 `VRTC`（pin28） | PCF85063 `VDD` 及 C23/C36 |
| `$1N371` | CH334P 内部 3.3 V LDO 输出（pin16） | 仅 C35/C51 去耦，不对外供电 |
| `$1N245` | CH343P 内部 LDO 输出（`V3`，pin6） | 仅 C34/C50 去耦 |
| `$1N171` | AXP2101 `VREF`（pin2） | C24 1 µF |

AXP2101 电源轨控制（寄存器依据 XPowersLib / AXP2101 数据手册）：

| 电源轨 | 使能寄存器 | 使能位 | 电压寄存器 | 电压码 | 3.3 V 写入值 |
|--------|-----------|:------:|-----------|--------|:------------:|
| `VCC`（DC1） | `0x80` | bit0 | `0x82` | (mV−1500)/100 | `0x12` |
| `VAUDIO`（DC3） | `0x80` | **bit2** | `0x84` | 1.6–3.4 V：88+(mV−1600)/100 | `0x69` |
| `AVCC`（ALDO3） | `0x90` | **bit2** | `0x94` | (mV−500)/100 | `0x1C` |

- **上电默认 VAUDIO(DC3) 未开启**，MAX98357A 无供电 → 必须显式使能。
- `AVCC`(ALDO3) 也需显式打开，否则 ICM-42670 / QMC5883P / AHT20 / 麦克风全部无响应。
- ADC 使能 `0x30`：bit0=VBAT，bit3=VSYS；VBAT=`0x34/0x35`，VBUS=`0x38/0x39`，VSYS=`0x3A/0x3B`，电量=`0xA4`。
- 关机：`0x10` bit0 写 1。
- 写电压寄存器时保留 bit7（DC 工作模式），只改低 7 位。

其他电源相关细节：

- **充电指示**：U14 `CHGLED`(pin1) 开漏拉低 → LED1 红色 LED（阳极经 R19 10 kΩ 接 `VSYS`）。
- **电池温度**：U14 `TS`(pin31) 经 R20 10 kΩ 接地（不使用 NTC）。
- **开机时序**：U14 `PWROK`(pin29) → ESP32 `CHIP_PU`，R2 10 kΩ 上拉；`PWRON`(pin30) 接电源键。
- **电源键不再等于 BOOT**：`R107` 已拆，`GPIO0` 与 `PWRON` 断开；GPIO0 由外部 10 kΩ 上拉到 VCC。
  需要进入下载模式时使用 USB-Serial-JTAG（`idf_monitor` 的 `Ctrl+T Ctrl+R`）或测试点。
- ESP32 `VDD_SPI`(pin29) 直接接 `VCC`，因此 Flash 为 3.3 V 供电。

---

## 7. 主要器件清单

| 位号 | 型号 | 功能 | 接口 |
|------|------|------|------|
| U1 | CH343P | USB ↔ UART 桥 | USB / UART0 |
| U2 | ESP32-S3R8 | 主控（8 MB 八线 PSRAM） | — |
| U3 | ICM-42670-P | 六轴 IMU（加速度 + 陀螺） | SPI |
| U4 | QMC5883P | 三轴磁力计 | I2C |
| U5 | W25Q128JVPIQ | 16 MB NOR Flash | SPI0/1 |
| U6 | AHT20 | 温湿度传感器 | I2C |
| U8 | CH334P | USB 2.0 Hub（1 上行 / 2 下行） | USB |
| U9 | MSM261S4030H0R | MEMS 麦克风 | I2S |
| U10 | MAX98357AETE+T | I2S 数字功放 | I2S |
| U11 | PCF85063ATL | RTC + X1 32.768 kHz | I2C |
| U12 | RFANT5220110A0T | 2.4 GHz 陶瓷天线 | RF |
| U13 | 2N7002K | 背光低边开关 | GPIO39 |
| U14 | AXP2101 | 电源管理 PMIC | I2C + IRQ |
| U15 | X252040MMB4SI | 40 MHz 主晶振 | XTAL |
| X4 | TXM12M0004252 | 12 MHz（CH334P 用） | — |
| CN1 | YXT-BB10-24S-02 | 24P FPC（屏 + 触摸） | SPI + I2C |
| USB1 | HX TYPE-C 16PIN | USB-C 接口 | USB |
| SW3 | TM-2024A | 导航/侧键开关 | GPIO |
| LED1 | NCD0402R1 | 红色充电指示灯 | CHGLED |
| TP1 / TP2 | — | `SPEAKER_N` / `SPEAKER_P` | 测试点 |
| TP3 / TP4 | — | `GND` / `VBAT` | 测试点 |


---

## 8. 使用注意事项

### 8.1 Strapping 引脚

| 引脚 | 作用 | 本设计处理 |
|------|------|-----------|
| GPIO0 | 启动模式（1 = SPI 启动，0 = 下载模式） | R107 已拆；外部 10 kΩ 上拉到 VCC + 内部弱上拉 |
| GPIO45 | `VDD_SPI` 电压选择（0 = 3.3 V，1 = 1.8 V） | 本设计 `VDD_SPI` 直接接 VCC，复位时须为 0；内部弱下拉满足 |
| GPIO46 | 启动模式（必须为 0 才能 SPI 启动） | R10 100 kΩ 外部下拉 |

> GPIO45 上还接了 `KEY_LEFT`（按键到地）。上电时按住左键不会改变 strapping 结果（仍为 0），
> 但**不要在 GPIO45 上增加外部上拉**，否则会改变 `VDD_SPI` 电压选择。

### 8.2 不能随便用的引脚

- **GPIO33 ~ GPIO37**：ESP32-S3R8 的片内八线 PSRAM 占用，任何情况下都不要配置。
- **GPIO27 ~ GPIO32**：SPI0/1 Flash 总线，`esp_flash` 驱动占用。
- **GPIO19 / GPIO20**：默认 USB-JTAG，若改作普通 GPIO 会失去 USB-JTAG 调试功能。
- **GPIO43 / GPIO44**：UART0（下载 + 日志），一般保留给控制台。

### 8.3 上拉 / 下拉

- `KEY_LEFT`(GPIO45)、`KEY_RIGHT`(GPIO40) **没有外部上拉**，代码里必须
  `pull_up_en = GPIO_PULLUP_ENABLE`，否则读值会浮空。
- `PMIC_INT`(GPIO41)、`RTC_INT`(GPIO42) 已有 10 kΩ 外部上拉，内部上拉可不启用；`PMIC_INT` 的上拉必须接常电域，不能接会被关掉的 VCC。
- 两路 I2C 均有 4.7 kΩ 外部上拉，使用 `GPIO_PULLUP_DISABLE` 即可。
- `I2S_SD`(GPIO7) 有 100 kΩ 下拉；`LCD_TOUCH_INT`(GPIO15) 无外部上下拉。

### 8.4 电平与命名

- `LCD_SDA` / `LCD_SCL` 是 SPI 的 MOSI / CLK，不要误当成 I2C。
- `LCD_BLK` 高电平点亮背光（N-MOS 低边开关）；低电平时背光 LED 回路断开。
- 触摸 I2C（GPIO12/13）与传感器 I2C（GPIO9/10）彼此独立。
- ESP32-S3 的 RTC GPIO 只有 GPIO0 ~ GPIO21；需要深睡唤醒的信号应选在这些引脚上
  （`LCD_TOUCH_INT` = GPIO15、`LCD_TE` = GPIO21 均满足；两个按键 GPIO40/45 不满足）。

### 8.5 硬件连接小提示

- `PWRON` 与 GPIO0 的连接（R107）已拆；电源键只接 AXP2101 `PWRON`，GPIO0 单独用 10 kΩ 上拉做 strapping。
  固件通过 AXP2101 的寄存器与 IRQ 处理长按/短按；
  **不要把 GPIO45 / GPIO46 配置成输出，也不要给它们增加外部上拉。**
- CH334P、CH343P 的 3.3 V 引脚是内部 LDO 输出，**不要外部供电**。
- `AVCC`、`VAUDIO`、`VCC` 的电压由 AXP2101 寄存器配置，上电后建议先读回确认。

---

## 9. 代码示例

```c
#include "board_pins.h"
#include "driver/spi_master.h"   /* 仅为下面的 SPI 示例 */

/* 按键：必须启用内部上拉 */
static void keys_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << GPIO_KEY_LEFT) | (1ULL << GPIO_KEY_RIGHT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

/* 背光：高电平点亮，可用 LEDC 在 GPIO_LCD_BLK 上做 PWM */
static void backlight_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_LCD_BLK,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(GPIO_LCD_BLK, 0);   /* 默认关背光 */
}

/* SPI 总线初始化示意（IMU） */
spi_bus_config_t bus = {
    .mosi_io_num = GPIO_IMU_MOSI,
    .miso_io_num = GPIO_IMU_MISO,
    .sclk_io_num = GPIO_IMU_SCK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4096,
};
```

---

## 10. 网表解析方法（可复现）

`.enet` 是 JSON 文件，顶层只有 `version / components / designRule / differentialPair /
netClass / equalLengthNetGroup`，**没有独立的 nets 段**；网络信息藏在每个元件的
`pinInfoMap` 里：

```jsonc
"components": {
  "gge2": {                       // 元件唯一 ID
    "props": { "Designator": "U2", "Manufacturer Part": "ESP32-S3R8", ... },
    "pinInfoMap": {
      "44": { "name": "MTCK", "number": "44", "net": "LCD_BLK", "props": {...} },
      ...
    }
  }
}
```

因此提取“网络 → 引脚”关系的核心逻辑只有三层循环：

```python
import json, collections

d = json.load(open('Doc/Netlist_Schematic1_2026-11-18.enet'))
nets = collections.OrderedDict()

for comp in d['components'].values():
    des = comp['props'].get('Designator', '?')
    for pin in comp.get('pinInfoMap', {}).values():
        net = pin.get('net', '').strip()
        if net:                       # 过滤掉未连接的引脚
            nets.setdefault(net, []).append((des, pin['name'], pin['number']))

for net, members in nets.items():
    print(net, members)
```

建议过滤规则：

- 只看非空 `net`，可去掉 `GND` / `VCC` / `AVCC` / `VBUS` 等电源网络以缩短输出；
- 只看 `props.Designator` 以 `U` / `CN` / `USB` / `SW` / `LED` / `TP` 开头的元件，
  即可忽略电容、电感、电阻的采购属性等冗余字段；
- 但**不要忽略电阻**，因为上下拉、0 Ω 跳线、串联电阻会改变引脚的实际用途
  （本设计中的 R107 已拆、GPIO0 需外部 10 kΩ 上拉，R10 下拉影响启动行为）。

---

## 附：网络名 → GPIO 速查

| 网络名 | GPIO | 网络名 | GPIO |
|--------|-----:|--------|-----:|
| `IMU_MISO` | 1 | `LCD_TOUCH_SCL` | 12 |
| `IMU_MOSI` | 2 | `LCD_TOUCH_SDA` | 13 |
| `IMU_SCK` | 3 | `LCD_TOUCH_RST` | 14 |
| `IMU_CS` | 4 | `LCD_TOUCH_INT` | 15 |
| `I2S_DIN` | 5 | `LCD_SDA` | 16 |
| `I2S_SCK` | 6 | `LCD_SCL` | 17 |
| `I2S_SD` | 7 | `LCD_CS` | 18 |
| `I2S_WS` | 8 | `ESP_DM` | 19 |
| `IIC_SCL` | 9 | `ESP_DP` | 20 |
| `IIC_SDA` | 10 | `LCD_TE` | 21 |
| `LCD_DC` | 11 | `LCD_RST` | 38 |
| `LCD_BLK` | 39 | `KEY_RIGHT` | 40 |
| `PMIC_INT` | 41 | `RTC_INT` | 42 |
| `ESP32_TX` | 43 | `ESP32_RX` | 44 |
| `KEY_LEFT` | 45 | `$1N30`（GPIO46 下拉） | 46 |

Flash 专用：`FLASH_SPI_HD`=27、`FLASH_SPI_WP`=28、`FLASH_SPI_CS0`=29、
`FLASH_SPI_CLK`=30、`FLASH_SPI_Q`=31、`FLASH_SPI_D`=32。


---

## 附二：工厂测试固件（factorytest/）

- 位置：`factorytest/`，以标准 ESP-IDF 组件加入主工程；随主工程一起构建，不是独立工程。
- 开关：`CONFIG_FACTORY_TEST_MODE`（默认 y），入口 `factory_test_start()`。
- 编译/烧录（工程根目录）：`idf.py build`、`idf.py -p /dev/ttyACM2 flash monitor`。
- 控制台：默认 **USB-Serial-JTAG 主控制台**（`/dev/ttyACMx`）；REPL 跟随 `CONFIG_ESP_CONSOLE_*`，
  需要 CH343P 时在 menuconfig 把主控制台改为 UART0。第二控制台只输出、不读输入。
- 启动时自动完成：I2C 初始化 → AXP2101 校验（ID 0x4A）→ DC1/ALDO3/**DC3(VAUDIO)** 3.3 V 使能。
- 常用命令：
  - `selftest`：PMIC / I2C / IMU / 磁力计 / 温湿度 / RTC / 触摸 / Flash / 按键 一键检查
  - `pmic rail`、`pmic on|off vaudio`、`pmic set avcc 3300`
  - `lcd res 240 284 0 0`、`lcd mode 3`、`lcd speed 20`、`lcd bars|grad|grid`、`lcd bright 0..100`
  - `touch stream 200`（在屏幕上画触摸轨迹）
  - `audio mic 5` / `audio tone 3` / `audio echo 10 8`（麦克风回声）
- 详细命令、自检判据与排障见 `factorytest/README.md`。

