#include "watch_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ft_lcd.h"
#include "ft_pmic.h"
#include "ft_sensors.h"
#include "watch_cloud.h"
#include "watch_audio.h"
#include "watch_gfx.h"
#include "watch_config.h"
#include "watch_net.h"
#include "watch_power.h"
#include "watch_script.h"
#include "watch_sensors.h"
#include "watch_storage.h"
#include "watch_term.h"
#include "watch_theme.h"
#include "watch_ui.h"

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    ws_net_status_t n;
    watch_net_status(&n);
    ws_power_t p = watch_sensors_power();
    printf("wifi   %s  ip=%s  rssi=%d  ap=%d\n", n.connected ? "up" : "down",
           n.ip, n.rssi, n.ap_mode);
    printf("battery %d%%  %d mV  vsys %d mV\n", p.batt_pct, p.batt_mv, p.vsys_mv);
    printf("storage %s  cloud %s\n", watch_storage_ready() ? "mounted" : "failed",
           watch_cloud_state_name(watch_cloud_state()));
    printf("theme %s  egg=%s  idle=%ds  vbus=%d\n",
           watch_theme_name(), watch_theme_error_unlocked() ? "unlocked" : "locked",
           watch_power_idle_seconds(), ft_pmic_vbus_present());
    printf("heap %u  psram %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifi <ssid> [password]\n");
        return 1;
    }
    watch_config_set_wifi(argv[1], argc >= 3 ? argv[2] : "");
    printf("wifi saved; rebooting...\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int cmd_ai(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ai <ws://host/path> [token]\n");
        return 1;
    }
    watch_config_set_ai(argv[1], argc >= 3 ? argv[2] : "");
    watch_cloud_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    watch_cloud_start();
    printf("ai endpoint saved; reconnect requested\n");
    return 0;
}

static int cmd_portal(int argc, char **argv)
{
    (void)argc; (void)argv;
    watch_net_start_portal();
    printf("portal: http://%s/  AP=%s\n", watch_net_ip(), CONFIG_WATCH_AP_SSID);
    return 0;
}

static int cmd_open(int argc, char **argv)
{
    if (argc < 2 || !watch_ui_open_app_by_name(argv[1])) {
        printf("usage: open watchface|ai|shell|sensors|media|games|settings\n");
        return 1;
    }
    return 0;
}

static int cmd_tap(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: tap <x> <y>\n");
        return 1;
    }
    int x = (int)strtol(argv[1], NULL, 0);
    int y = (int)strtol(argv[2], NULL, 0);
    if (!watch_ui_tap(x, y)) {
        printf("tap queue full\n");
        return 1;
    }
    return 0;
}

static int cmd_chat(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: chat <text>\n");
        return 1;
    }
    if (!watch_cloud_send_chat(argv[1])) {
        printf("chat failed (cloud offline or capture active)\n");
        return 1;
    }
    return 0;
}

static int cmd_script(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: script <file.wsh>\n");
        return 1;
    }
    char path[128];
    if (argv[1][0] == '/') snprintf(path, sizeof(path), "%s", argv[1]);
    else snprintf(path, sizeof(path), "%s/%s", WS_SCRIPT_DIR, argv[1]);
    if (!watch_script_execute(path)) {
        printf("script busy or not found\n");
        return 1;
    }
    return 0;
}

static int cmd_term(int argc, char **argv)
{
    (void)argc; (void)argv;
    watch_term_dump();
    return 0;
}

static int cmd_test(int argc, char **argv)
{
    (void)argc; (void)argv;
    watch_ui_pause_render();
    vTaskDelay(pdMS_TO_TICKS(100));
    watch_gfx_clear(WS_COLOR_BLACK);
    watch_gfx_text(WS_SAFE, 8, "ICON TEST", WS_COLOR_WHITE, 1);
    watch_gfx_circle(120, 140, 46, WS_COLOR_WHITE);
    watch_gfx_icon(WS_APP_WATCHFACE, 120, 140, 100, 0, WS_COLOR_GREEN);
    watch_gfx_flush();
    return 0;
}

static int cmd_shot(int argc, char **argv)
{
    (void)argc; (void)argv;
    const ft_lcd_info_t *info = ft_lcd_info();
    const uint16_t *fb = ft_lcd_framebuffer();
    if (!info || !fb) {
        printf("no framebuffer\n");
        return 1;
    }
    size_t bytes = (size_t)info->width * info->height * sizeof(uint16_t);
    uint16_t *snapshot = malloc(bytes);
    if (!snapshot) {
        printf("no snapshot memory\n");
        return 1;
    }
    watch_ui_pause_render();
    vTaskDelay(pdMS_TO_TICKS(200));
    memcpy(snapshot, fb, bytes);
    watch_ui_resume_render();

    printf("SHOT %u %u 2\n", info->width, info->height);
    fflush(stdout);
    const uint8_t *bytes_out = (const uint8_t *)snapshot;
    size_t total = bytes;
    /* USB serial console can drop large binary bursts (the AI screenshot is
     * 136 KB).  Pace small chunks so `shot` is reliable over a plain 115200
     * UART as well as USB CDC. */
    for (size_t off = 0; off < total; off += 128) {
        size_t n = total - off;
        if (n > 128) n = 128;
        fwrite(bytes_out + off, 1, n, stdout);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("\nSHOT_END\n");
    free(snapshot);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: cat <file.wsh>\n");
        return 1;
    }
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", WS_SCRIPT_DIR, argv[1]);
    char *buf = NULL;
    size_t len = 0;
    if (watch_storage_read_all(path, &buf, &len) != ESP_OK) {
        printf("read failed: %s\n", watch_storage_error());
        return 1;
    }
    fwrite(buf, 1, len, stdout);
    free(buf);
    return 0;
}

static int cmd_rtc(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "set")) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        if (argc < 4 ||
            sscanf(argv[2], "%d-%d-%d", &y, &mo, &d) != 3 ||
            sscanf(argv[3], "%d:%d:%d", &h, &mi, &s) != 3) {
            printf("usage: rtc set YYYY-MM-DD HH:MM:SS\n");
            return 1;
        }
        esp_err_t err = ft_rtc_set_datetime(y, mo, d, h, mi, s);
        printf("rtc set: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    ft_rtc_raw_t raw = {0};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    esp_err_t err = ft_rtc_read(&raw);
    if (err == ESP_OK) {
        ft_rtc_to_decimal(&raw, &y, &mo, &d, &h, &mi, &s);
        printf("rtc %04d-%02d-%02d %02d:%02d:%02d  valid=%d\n",
               y, mo, d, h, mi, s, ft_rtc_is_valid());
    } else {
        printf("rtc read failed: %s\n", esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

static int cmd_theme(int argc, char **argv)
{
    if (argc < 2) {
        printf("theme: %s (egg %s)\n", watch_theme_name(),
               watch_theme_error_unlocked() ? "unlocked" : "locked");
        printf("usage: theme kali|chaos|error|reset\n");
        return 0;
    }
    if (!strcmp(argv[1], "kali")) {
        watch_theme_set(WS_THEME_KALI);
    } else if (!strcmp(argv[1], "chaos")) {
        watch_theme_set(WS_THEME_CHAOS);
    } else if (!strcmp(argv[1], "error")) {
        watch_theme_unlock_error();
    } else if (!strcmp(argv[1], "reset")) {
        watch_theme_disable_error();
    } else {
        printf("usage: theme kali|chaos|error|reset\n");
        return 1;
    }
    printf("theme now %s\n", watch_theme_name());
    return 0;
}

static int cmd_poweroff(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("power off (active: next boot normal)\n");
    watch_power_off(WS_POWEROFF_ACTIVE);
    return 0;
}

static int cmd_pmic(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "irq")) {
        ft_pmic_print_irq();
        return 0;
    }
    printf("usage: pmic irq\n");
    return 1;
}

static int cmd_idle(int argc, char **argv)
{
    if (argc < 2) {
        printf("idle timeout: %ds (0=off)\n", watch_power_idle_seconds());
        printf("usage: idle <seconds>\n");
        return 0;
    }
    int sec = atoi(argv[1]);
    if (sec < 0) sec = 0;
    watch_power_set_idle_seconds(sec);
    printf("idle timeout set: %ds\n", watch_power_idle_seconds());
    return 0;
}

static int cmd_bootmode(int argc, char **argv)
{
    if (argc < 2) {
        printf("next boot: %s\n",
               watch_config_get_int("boot_fast", 0) ? "fast" : "normal");
        printf("usage: bootmode fast|normal (then reboot)\n");
        return 0;
    }
    if (!strcmp(argv[1], "fast")) {
        watch_config_set_int("boot_fast", 1);
    } else if (!strcmp(argv[1], "normal")) {
        watch_config_set_int("boot_fast", 0);
    } else {
        printf("usage: bootmode fast|normal\n");
        return 1;
    }
    printf("next boot mode set: %s\n", argv[1]);
    return 0;
}

static int cmd_ptt(int argc, char **argv)
{
    int ms = argc >= 2 ? atoi(argv[1]) : 5000;
    if (ms < 500) ms = 500;
    if (ms > 15000) ms = 15000;
    if (!watch_cloud_connected()) {
        printf("cloud offline\n");
        return 1;
    }
    printf("PTT %d ms\n", ms);
    watch_cloud_listen(true);
    vTaskDelay(pdMS_TO_TICKS(ms));
    watch_cloud_listen(false);
    printf("PTT done\n");
    return 0;
}

static int cmd_echotest(int argc, char **argv)
{
    int ms = argc >= 2 ? atoi(argv[1]) : 3000;
    if (ms < 500) ms = 500;
    if (ms > 10000) ms = 10000;
    if (!watch_cloud_connected()) {
        printf("cloud offline\n");
        return 1;
    }
    printf("echo test: record + 1kHz tone %d ms\n", ms);
    watch_cloud_listen(true);
    vTaskDelay(pdMS_TO_TICKS(300));
    watch_audio_tone(1000, ms);
    vTaskDelay(pdMS_TO_TICKS(300));
    watch_cloud_listen(false);
    printf("echo test done\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_restart();
    return 0;
}

void watch_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "esp-watch>";
    repl_cfg.max_cmdline_length = 160;
    repl_cfg.task_stack_size = 8192;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
#else
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl);
#endif
    if (err != ESP_OK) {
        printf("console unavailable: %s\n", esp_err_to_name(err));
        return;
    }

    const esp_console_cmd_t cmds[] = {
        { .command = "status", .help = "network / battery / storage / cloud", .func = cmd_status },
        { .command = "wifi", .help = "wifi <ssid> [password]", .func = cmd_wifi },
        { .command = "ai", .help = "ai <ws-url> [token]", .func = cmd_ai },
        { .command = "portal", .help = "start ESP-Watch setup AP", .func = cmd_portal },
        { .command = "open", .help = "open <app>", .func = cmd_open },
        { .command = "tap", .help = "tap <x> <y> (synthetic touch)", .func = cmd_tap },
        { .command = "chat", .help = "chat <text>", .func = cmd_chat },
        { .command = "script", .help = "script <file.wsh>", .func = cmd_script },
        { .command = "term", .help = "dump shell terminal buffer", .func = cmd_term },
        { .command = "cat", .help = "cat <file.wsh>", .func = cmd_cat },
        { .command = "shot", .help = "dump framebuffer (RGB565)", .func = cmd_shot },
        { .command = "test", .help = "draw icon test frame", .func = cmd_test },
        { .command = "rtc", .help = "rtc [set YYYY-MM-DD HH:MM:SS]", .func = cmd_rtc },
        { .command = "theme", .help = "theme kali|chaos|error|reset", .func = cmd_theme },
        { .command = "poweroff", .help = "PMIC power off (active)", .func = cmd_poweroff },
        { .command = "pmic", .help = "pmic irq", .func = cmd_pmic },
        { .command = "idle", .help = "idle [seconds, 0=off]", .func = cmd_idle },
        { .command = "bootmode", .help = "bootmode fast|normal (then reboot)", .func = cmd_bootmode },
        { .command = "ptt", .help = "ptt [ms] record and send to cloud AI", .func = cmd_ptt },
        { .command = "echotest", .help = "echotest [ms] record while playing 1kHz", .func = cmd_echotest },
        { .command = "reboot", .help = "reboot", .func = cmd_reboot },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    esp_console_start_repl(repl);
}
