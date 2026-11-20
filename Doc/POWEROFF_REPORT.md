# WatchShell 关机自动重启 / 电源键问题复盘

日期：2026-11-19  
适用硬件：ESP32-S3 + AXP2101（R107 已拆改板）

## 1. 现象

1. 系统主动关机后，设备很快自行重新上电，表现为“关机后立即自启”。
2. 关机后 `PWRON_STATUS` 中出现 **IRQ 低电平作为 POWERON source** 的记录。
3. 拆掉 R107 后，PWR 按键仍然通过 AXP2101 的 `PWRON → PMIC_INT(GPIO41)` 进入系统，但 GPIO0 失去原来的上拉路径。
4. 长按 PWR 不会弹出电源界面；长按 6 秒仍由 AXP2101 硬件断电。

## 2. 根因

### 2.1 IRQ 上拉源错误

AXP2101 的 `IRQ` 是开漏输出，数据手册要求外部上拉，且 **芯片内部没有 IRQ 上拉**。

原设计：

```text
AXP2101 IRQ ── R6 10 kΩ ── VCC
```

`VCC` 是 AXP2101 `DC1` 的输出。软件关机时 `DC1` 被切断：

```text
关机 → DC1/VCC 掉电 → IRQ 失去上拉 → IRQ 被拉低/悬空
     → 超过 16 ms → AXP2101 记录 IRQ low as POWERON source
     → PMIC 重新上电 → 设备自启
```

这与实测的 `PWRON_STATUS` 记录一致，是关机自启的主要根因。

**修复方向：**

```text
AXP2101 IRQ ── 4.7 kΩ/10 kΩ ── 常电域（VRTC/RTCLDO 3.0–3.3 V）
```

注意：

- 不要直接上拉到 `VSYS`，VSYS 最高约 4.2 V，超过 ESP32 GPIO 耐压。
- 如果使用 `VRTC`，先确认其为 3.0 V/3.3 V；如果是 1.8 V，需要先配置 RTCLDO。
- `RTC_INT` 的 R4 也建议跟 PCF85063 的供电域保持一致，接到 VRTC，而不是 VCC。

### 2.2 PWRON 上拉

AXP2101 `PWRON`(pin30) **内部有约 100 kΩ 上拉到 VINT**。原设计的外部 R15 10 kΩ 上拉到 VCC 是冗余的：

- PWRON 不依赖 R15 也能保持高电平；
- R15 接 VCC 后，关机时同样会失效；
- 保留 R15 不会解决 IRQ 问题，但会增加关机后 PWRON 网络的不确定性。

建议删除 R15，或把它的上拉改到常电域；不要继续接 DC1 的 VCC。

### 2.3 R107 拆除

原设计：

```text
GPIO0 ── R107(0 Ω) ── PWRON
```

R107 拆除后：

- GPIO0 不再与 PWRON 相连；
- GPIO0 只作为 ESP32-S3 的启动 strapping；
- **必须在 GPIO0 与 VCC 之间补 10 kΩ 上拉**；
- 否则 GPIO0 浮空，冷启动可能进入下载模式：
  `boot:0x0 (DOWNLOAD(USB/UART0))`
- 电源键不再兼作硬件 BOOT 键，下载改用 USB-Serial-JTAG 或测试点。

### 2.4 软件兜底

`watch_power_off()` 在 PMIC 断电失败或未真正切断电源时仍保留：

```c
esp_restart();
```

硬件上拉修复后，正常关机不会走到这里。但如果 PMIC 命令失败，这个兜底会表现为“立即重启”。后续可改为：

- 记录错误；
- 恢复背光；
- 显示“POWER OFF FAILED”；
- 保持运行，等待用户重试或继续按住 PWR 6 秒硬件关机。

## 3. 已完成的软件修改

### 3.1 长按电源菜单

在 `components/watchshell/src/watch_ui.c` 中新增：

```text
PWR 按住约 1.2 秒
  → POWER MENU
     POWER OFF
     REBOOT
     CANCEL
```

交互：

- 默认高亮 `CANCEL`，避免松手误关机；
- `LEFT/RIGHT` 循环选择；
- `PWR` 短按执行当前项；
- 触摸可直接点按按钮；
- 菜单打开后松开 PWR 不会触发转盘进入 App、App 返回或 Settings 执行；
- 继续按住到 6 秒由 AXP2101 硬件断电，软件菜单不拦截；
- 低电压警告出现时强制关闭菜单；
- 菜单打开后暂停当前 App 的 `tick()`，避免后台状态在模态菜单期间变化。

涉及文件：

```text
components/watchshell/src/watch_ui.c
components/watchshell/src/app_settings.c
README.md
components/watchshell/README.md
Doc/PINMAP.md
```

### 3.2 长按判定

```c
#define PWR_SHORT_MS       650
#define PWR_MENU_HOLD_MS   1200
```

- `< 650 ms`：保持原有短按语义；
- `650–1200 ms`：原有行为（Settings 退出，其他 App 不动作）；
- `>= 1200 ms`：打开电源菜单；
- 打开菜单的那一次按键释放会被消费，不会漏到后台 App。

## 4. 验证清单

- [ ] 冷启动日志为 `PWR source PMIC_INT`，GPIO0 静态为高。
- [ ] 软关机后不再自动重启；重复 10 次仍保持断电。
- [ ] 长按约 1.2 秒弹出 POWER MENU，松手菜单保留。
- [ ] 菜单中 `LEFT/RIGHT` 可循环选择，PWR 可执行。
- [ ] 触摸 POWER OFF / REBOOT / CANCEL 均有效。
- [ ] 直接按住 6 秒，AXP2101 硬件断电优先。
- [ ] 低电压警告期间不弹菜单。
- [ ] `PWRON_STATUS` 不再出现 IRQ low as POWERON source。

## 5. 待确认

- 实际改板中 R6 连接的常电网络名称和电压（VRTC / RTCLDO / 其他 3.0–3.3 V）。
- GPIO0 的 10 kΩ 外部上拉是否已经补上。
- R15 是删除还是改接常电域。
- 是否移除 `watch_power_off()` 中无条件的 `esp_restart()` 兜底。
