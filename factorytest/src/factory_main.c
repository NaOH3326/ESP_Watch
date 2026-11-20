/**
 * @file  factory_main.c
 * @brief ESP_Watch 工厂测试工具：交互式 REPL + 一键自检
 */
#include "ft_board.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "factory_test.h"
#include "ft_audio.h"
#include "ft_i2c.h"
#include "ft_keys.h"
#include "ft_lcd.h"
#include "ft_pmic.h"
#include "ft_sensors.h"
#include "ft_system.h"
#include "ft_touch.h"
#include "ft_bt.h"
#include "ft_wifi.h"

ft_presence_t g_ft_present;

typedef struct {
    const char *name;
    const char *help;
    int (*fn)(int argc, char **argv);
} ft_cmd_t;

static int cmd_help(int argc, char **argv);
static int cmd_info(int argc, char **argv);
static int cmd_selftest(int argc, char **argv);
static int cmd_i2c(int argc, char **argv);
static int cmd_pmic(int argc, char **argv);
static int cmd_imu(int argc, char **argv);
static int cmd_mag(int argc, char **argv);
static int cmd_aht(int argc, char **argv);
static int cmd_rtc(int argc, char **argv);
static int cmd_lcd(int argc, char **argv);
static int cmd_touch(int argc, char **argv);
static int cmd_keys(int argc, char **argv);
static int cmd_audio(int argc, char **argv);
static int cmd_wifi(int argc, char **argv);
static int cmd_bt(int argc, char **argv);
static int cmd_flash(int argc, char **argv);
static int cmd_usb(int argc, char **argv);
static int cmd_reboot(int argc, char **argv);
static int cmd_poweroff(int argc, char **argv);

static const ft_cmd_t s_cmds[] = {
    { "help",     "列出所有命令",                                   cmd_help },
    { "info",     "芯片/电源/屏幕/触摸/传感器概览",                 cmd_info },
    { "selftest", "一键自检（不含音频与目视项）",                   cmd_selftest },
    { "i2c",      "i2c scan [sensor|touch|all]",                    cmd_i2c },
    { "pmic",     "pmic info|init|rail|adc|on|off|set|poweroff",    cmd_pmic },
    { "imu",      "imu id|read|stream [n]",                         cmd_imu },
    { "mag",      "mag id",                                         cmd_mag },
    { "aht",      "aht read",                                       cmd_aht },
    { "rtc",      "rtc read|set|tick|int",                          cmd_rtc },
    { "lcd",      "lcd init|info|res|rot|mode|speed|bright|on|off|fill|bars|grad|grid|pattern|text", cmd_lcd },
    { "touch",    "touch id|read|stream [n]|trace [sec]|int",          cmd_touch },
    { "keys",     "keys [stream [sec]]",                            cmd_keys },
    { "audio",    "audio mic|tone|echo [sec] [gain]",               cmd_audio },
    { "wifi",     "wifi scan [n]|connect <ssid> [pass]|status",     cmd_wifi },
    { "bt",       "bt scan [sec]",                                  cmd_bt },
    { "flash",    "Flash JEDEC ID / 容量",                          cmd_flash },
    { "usb",      "USB-Serial-JTAG 连接状态",                       cmd_usb },
    { "reboot",   "重启",                                           cmd_reboot },
    { "poweroff", "AXP2101 关机",                                   cmd_poweroff },
};

static const size_t s_cmds_len = sizeof(s_cmds) / sizeof(s_cmds[0]);

static const char *TAG = "FT_MAIN";

static int parse_rail(const char *s, ft_pmic_rail_t *rail)
{
    if (!s || !rail) {
        return -1;
    }
    if (!strcasecmp(s, "vcc")) {
        *rail = FT_RAIL_VCC;
    } else if (!strcasecmp(s, "avcc")) {
        *rail = FT_RAIL_AVCC;
    } else if (!strcasecmp(s, "vaudio")) {
        *rail = FT_RAIL_VAUDIO;
    } else {
        return -1;
    }
    return 0;
}

static void print_banner(void)
{
    const ft_lcd_info_t *lcd = ft_lcd_info();
    printf("\n");
    printf("==================================================\n");
    printf("  ESP_Watch Factory Test\n");
    printf("  ESP-IDF %s\n", esp_get_idf_version());
    printf("  PMIC: %s   LCD: %s %ux%u   Touch: CST816D\n",
           g_ft_present.pmic ? "AXP2101 OK" : "NOT FOUND",
           ft_lcd_controller_name(), lcd->width, lcd->height);
    printf("  Type 'help' for commands, 'selftest' for auto test.\n");
    printf("==================================================\n\n");
}

static int cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("命令列表:\n");
    for (size_t i = 0; i < s_cmds_len; i++) {
        printf("  %-10s %s\n", s_cmds[i].name, s_cmds[i].help);
    }
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const ft_lcd_info_t *lcd = ft_lcd_info();

    printf("=== System ===\n");
    ft_system_print_info();
    printf("=== PMIC ===\n");
    ft_pmic_print_info();
    printf("=== LCD ===\n");
    printf("  %s %ux%u offset(%u,%u) rotation=%u brightness=%u%% fb=%s\n",
           ft_lcd_controller_name(), lcd->width, lcd->height, lcd->x_offset, lcd->y_offset,
           lcd->rotation, lcd->brightness, lcd->have_fb ? "yes" : "no");
    printf("=== Touch ===\n");
    if (ft_touch_present()) {
        printf("  CST816D id=0x%02X fw=0x%02X INT=%s\n", ft_touch_chip_id(), ft_touch_fw_version(),
               ft_touch_int_level() ? "high" : "low");
    } else {
        printf("  CST816D not found (addr 0x%02X)\n", FT_I2C_ADDR_CST816D);
    }
    printf("=== Sensors ===\n");
    uint8_t who = 0;
    if (ft_imu_probe(&who) == ESP_OK) {
        printf("  ICM-42670-P WHO_AM_I=0x%02X (expect 0x67)\n", who);
    } else {
        printf("  ICM-42670-P not responding\n");
    }
    uint8_t mid = 0, mst = 0;
    if (ft_mag_probe(&mid, &mst) == ESP_OK) {
        printf("  QMC5883P CHIP_ID=0x%02X STATUS=0x%02X (expect 0x80)\n", mid, mst);
    } else {
        printf("  QMC5883P not responding\n");
    }
    uint8_t ast = 0;
    if (ft_aht_probe(&ast) == ESP_OK) {
        printf("  AHT20 STATUS=0x%02X\n", ast);
    } else {
        printf("  AHT20 not responding\n");
    }
    uint8_t ctrl1 = 0;
    if (ft_rtc_probe(&ctrl1) == ESP_OK) {
        ft_rtc_raw_t raw = {0};
        ft_rtc_read(&raw);
        int y, mo, d, h, mi, s;
        ft_rtc_to_decimal(&raw, &y, &mo, &d, &h, &mi, &s);
        printf("  PCF85063 CTRL1=0x%02X time=%04d-%02d-%02d %02d:%02d:%02d\n",
               ctrl1, y, mo, d, h, mi, s);
    } else {
        printf("  PCF85063 not responding\n");
    }
    ft_keys_print();
    return 0;
}


static int cmd_selftest(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int pass = 0, fail = 0;
    char detail[96];

    printf("\n===== Factory Self Test =====\n");

#define REPORT(name, ok)                                                  \
    do {                                                                  \
        printf("  [%s] %-22s %s\n", (ok) ? " OK " : "FAIL", name, detail); \
        if (ok) {                                                         \
            pass++;                                                       \
        } else {                                                          \
            fail++;                                                       \
        }                                                                 \
    } while (0)

    int pid = ft_pmic_chip_id();
    bool pmic_ok = (pid == 0x4A) && ft_pmic_rail_is_on(FT_RAIL_VCC) &&
                   ft_pmic_rail_is_on(FT_RAIL_AVCC) && ft_pmic_rail_is_on(FT_RAIL_VAUDIO);
    snprintf(detail, sizeof(detail), "id=0x%02X VCC/AVCC/VAUDIO=%s", pid,
             pmic_ok ? "ON" : "CHECK");
    REPORT("PMIC AXP2101", pmic_ok);

    bool i2c_ok = ft_i2c_probe(FT_I2C_ADDR_AXP2101, false) &&
                  ft_i2c_probe(FT_I2C_ADDR_QMC5883P, false) &&
                  ft_i2c_probe(FT_I2C_ADDR_AHT20, false) &&
                  ft_i2c_probe(FT_I2C_ADDR_PCF85063, false);
    snprintf(detail, sizeof(detail), "0x34 / 0x2C / 0x38 / 0x51");
    REPORT("Sensor I2C bus", i2c_ok);

    uint8_t who = 0;
    bool imu_ok = (ft_imu_probe(&who) == ESP_OK) && (who == 0x67);
    snprintf(detail, sizeof(detail), "WHO_AM_I=0x%02X (expect 0x67)", who);
    REPORT("IMU ICM-42670-P", imu_ok);

    uint8_t mid = 0, mst = 0;
    bool mag_ok = (ft_mag_probe(&mid, &mst) == ESP_OK) && (mid == 0x80);
    snprintf(detail, sizeof(detail), "CHIP_ID=0x%02X (expect 0x80)", mid);
    REPORT("MAG QMC5883P", mag_ok);

    uint8_t ast = 0;
    bool aht_ok = (ft_aht_probe(&ast) == ESP_OK);
    float t = 0, rh = 0;
    if (aht_ok && ft_aht_read(&t, &rh) == ESP_OK) {
        snprintf(detail, sizeof(detail), "status=0x%02X %.1fC %.1f%%", ast, t, rh);
    } else {
        snprintf(detail, sizeof(detail), "status=0x%02X", ast);
    }
    REPORT("T/H AHT20", aht_ok);

    uint8_t ctrl1 = 0;
    bool rtc_ok = (ft_rtc_probe(&ctrl1) == ESP_OK);
    int tick = 0;
    if (rtc_ok && ft_rtc_check_tick(&tick) != ESP_OK) {
        ft_rtc_set_default();
        rtc_ok = (ft_rtc_check_tick(&tick) == ESP_OK);
    }
    snprintf(detail, sizeof(detail), "CTRL1=0x%02X tick=%ds", ctrl1, tick);
    REPORT("RTC PCF85063", rtc_ok);

    bool touch_ok = ft_touch_present() && (ft_touch_chip_id() != 0x00);
    snprintf(detail, sizeof(detail), "id=0x%02X fw=0x%02X", ft_touch_chip_id(),
             ft_touch_fw_version());
    REPORT("TOUCH CST816D", touch_ok);

    uint32_t fid = 0, fsize = 0;
    bool flash_ok = ft_flash_check(&fid, &fsize);
    snprintf(detail, sizeof(detail), "id=0x%06X %uMB", (unsigned)fid,
             (unsigned)(fsize / (1024 * 1024)));
    REPORT("Flash W25Q128", flash_ok && fid == 0xEF4018);

    snprintf(detail, sizeof(detail), "PWR=%d LEFT=%d RIGHT=%d",
             ft_keys_pressed(FT_KEY_PWR), ft_keys_pressed(FT_KEY_LEFT),
             ft_keys_pressed(FT_KEY_RIGHT));
    REPORT("Keys", true);

#undef REPORT

    printf("  [MANUAL] %-19s 请目视确认显示/背光/触摸\n", "LCD + Touch");
    printf("--------------------------------\n");
    printf("  Result: %d passed, %d failed, 1 manual\n\n", pass, fail);
    return fail ? 1 : 0;
}

static int cmd_i2c(int argc, char **argv)
{
    const char *which = (argc > 2) ? argv[2] : "all";
    if (!strcasecmp(which, "sensor") || !strcasecmp(which, "all")) {
        int n = ft_i2c_scan(false);
        printf("  sensor bus: %d device(s)\n", n);
    }
    if (!strcasecmp(which, "touch") || !strcasecmp(which, "all")) {
        int n = ft_i2c_scan(true);
        printf("  touch bus: %d device(s)\n", n);
    }
    return 0;
}

static int cmd_pmic(int argc, char **argv)
{
    if (argc < 2 || !strcasecmp(argv[1], "info") || !strcasecmp(argv[1], "rail")) {
        ft_pmic_print_info();
        return 0;
    }
    if (!strcasecmp(argv[1], "init")) {
        esp_err_t err = ft_pmic_init();
        printf("  pmic init: %s\n", esp_err_to_name(err));
        ft_pmic_print_info();
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(argv[1], "adc")) {
        printf("  VBAT=%d mV  VSYS=%d mV  VBUS=%d mV  batt=%d%%\n",
               ft_pmic_read_batt_mv(), ft_pmic_read_vsys_mv(), ft_pmic_read_vbus_mv(),
               ft_pmic_battery_percent());
        return 0;
    }
    if (!strcasecmp(argv[1], "poweroff")) {
        ft_pmic_poweroff();
        return 0;
    }
    if (!strcasecmp(argv[1], "on") || !strcasecmp(argv[1], "off")) {
        if (argc < 3) {
            printf("  usage: pmic %s <vcc|avcc|vaudio>\n", argv[1]);
            return 1;
        }
        ft_pmic_rail_t rail;
        if (parse_rail(argv[2], &rail) != 0) {
            printf("  unknown rail: %s\n", argv[2]);
            return 1;
        }
        bool on = !strcasecmp(argv[1], "on");
        esp_err_t err = ft_pmic_rail_enable(rail, on);
        printf("  %s %s: %s\n", ft_pmic_rail_name(rail), on ? "ON" : "OFF",
               esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(argv[1], "set")) {
        if (argc < 4) {
            printf("  usage: pmic set <vcc|avcc|vaudio> <mV>\n");
            return 1;
        }
        ft_pmic_rail_t rail;
        if (parse_rail(argv[2], &rail) != 0) {
            printf("  unknown rail: %s\n", argv[2]);
            return 1;
        }
        esp_err_t err = ft_pmic_set_voltage(rail, (uint16_t)atoi(argv[3]));
        printf("  %s <- %s mV: %s\n", ft_pmic_rail_name(rail), argv[3], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("  usage: pmic info|init|rail|adc|on|off|set|poweroff\n");
    return 1;
}


static int cmd_imu(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "id";
    uint8_t who = 0;
    esp_err_t err = ft_imu_probe(&who);
    if (err != ESP_OK) {
        printf("  IMU not responding: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (!strcasecmp(sub, "id")) {
        printf("  ICM-42670-P WHO_AM_I=0x%02X (expect 0x67)\n", who);
        return (who == 0x67) ? 0 : 1;
    }
    if (!strcasecmp(sub, "read") || !strcasecmp(sub, "stream")) {
        int n = (argc > 2) ? atoi(argv[2]) : 1;
        if (n <= 0) {
            n = 1;
        }
        for (int i = 0; i < n; i++) {
            ft_imu_sample_t s = {0};
            err = ft_imu_read(&s);
            if (err != ESP_OK) {
                printf("  read failed: %s\n", esp_err_to_name(err));
                return 1;
            }
            printf("  acc[g] %6.2f %6.2f %6.2f  gyro[dps] %7.1f %7.1f %7.1f  temp %.1fC\n",
                   s.accel_g[0], s.accel_g[1], s.accel_g[2],
                   s.gyro_dps[0], s.gyro_dps[1], s.gyro_dps[2], s.temp_c);
            if (n > 1 && i + 1 < n) {
                ft_delay_ms(100);
            }
        }
        return 0;
    }
    printf("  usage: imu id|read|stream [n]\n");
    return 1;
}

static int cmd_mag(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t id = 0, status = 0;
    esp_err_t err = ft_mag_probe(&id, &status);
    if (err != ESP_OK) {
        printf("  QMC5883P not responding: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("  QMC5883P CHIP_ID=0x%02X (expect 0x80) STATUS=0x%02X\n", id, status);
    return (id == 0x80) ? 0 : 1;
}

static int cmd_aht(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t status = 0;
    esp_err_t err = ft_aht_probe(&status);
    if (err != ESP_OK) {
        printf("  AHT20 not responding: %s\n", esp_err_to_name(err));
        return 1;
    }
    float t = 0, rh = 0;
    err = ft_aht_read(&t, &rh);
    if (err != ESP_OK) {
        printf("  AHT20 status=0x%02X, read failed: %s\n", status, esp_err_to_name(err));
        return 1;
    }
    printf("  AHT20 status=0x%02X  temperature=%.2f C  humidity=%.2f %%\n", status, t, rh);
    return 0;
}

static int cmd_rtc(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "read";
    if (!strcasecmp(sub, "set")) {
        esp_err_t err = ft_rtc_set_default();
        printf("  set 2026-11-18 12:00:00: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "tick")) {
        int delta = 0;
        esp_err_t err = ft_rtc_check_tick(&delta);
        printf("  tick check: %s, delta=%d s\n", esp_err_to_name(err), delta);
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "int")) {
        esp_err_t err = ft_rtc_timer_int_test();
        printf("  RTC timer interrupt: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    uint8_t ctrl1 = 0;
    esp_err_t err = ft_rtc_probe(&ctrl1);
    if (err != ESP_OK) {
        printf("  PCF85063 not responding: %s\n", esp_err_to_name(err));
        return 1;
    }
    ft_rtc_raw_t raw = {0};
    err = ft_rtc_read(&raw);
    if (err != ESP_OK) {
        printf("  read failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    int y, mo, d, h, mi, s;
    ft_rtc_to_decimal(&raw, &y, &mo, &d, &h, &mi, &s);
    printf("  PCF85063 CTRL1=0x%02X raw=%02X %02X %02X %02X %02X %02X %02X\n",
           ctrl1, raw.seconds, raw.minutes, raw.hours, raw.days, raw.weekdays, raw.months,
           raw.years);
    printf("  time=%04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
    return 0;
}


static uint16_t parse_color(const char *name)
{
    if (!name) {
        return 0xFFFF;
    }
    if (!strcasecmp(name, "red")) return 0xF800;
    if (!strcasecmp(name, "green")) return 0x07E0;
    if (!strcasecmp(name, "blue")) return 0x001F;
    if (!strcasecmp(name, "cyan")) return 0x07FF;
    if (!strcasecmp(name, "magenta")) return 0xF81F;
    if (!strcasecmp(name, "yellow")) return 0xFFE0;
    if (!strcasecmp(name, "gray")) return 0x8410;
    if (!strcasecmp(name, "black")) return 0x0000;
    if (!strcasecmp(name, "white")) return 0xFFFF;
    unsigned long v = strtoul(name, NULL, 0);
    if (v > 0xFFFF) {
        return ft_rgb565((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
    }
    return (uint16_t)v;
}

static int cmd_lcd(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "info";
    const ft_lcd_info_t *lcd = ft_lcd_info();

    if (!strcasecmp(sub, "init")) {
        esp_err_t err = ft_lcd_reinit();
        printf("  lcd init: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "info")) {
        printf("  ctrl=%s %ux%u offset(%u,%u) rot=%u bright=%u%% spi_mode=%u spi=%luMHz fb=%s\n",
               ft_lcd_controller_name(), lcd->width, lcd->height, lcd->x_offset, lcd->y_offset,
               lcd->rotation, lcd->brightness, lcd->spi_mode,
               (unsigned long)(lcd->spi_hz / 1000000), lcd->have_fb ? "yes" : "no");
        return 0;
    }
    if (!strcasecmp(sub, "res")) {
        if (argc < 4) {
            printf("  usage: lcd res <w> <h> [xoff] [yoff]\n");
            return 1;
        }
        uint16_t w = (uint16_t)atoi(argv[2]);
        uint16_t h = (uint16_t)atoi(argv[3]);
        uint16_t xo = (argc > 4) ? (uint16_t)atoi(argv[4]) : 0;
        uint16_t yo = (argc > 5) ? (uint16_t)atoi(argv[5]) : 0;
        esp_err_t err = ft_lcd_set_resolution(w, h, xo, yo);
        printf("  lcd res %ux%u+%u+%u: %s\n", w, h, xo, yo, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "rot")) {
        if (argc < 3) {
            printf("  usage: lcd rot <0-3>\n");
            return 1;
        }
        esp_err_t err = ft_lcd_set_rotation((uint8_t)atoi(argv[2]));
        printf("  lcd rot %s: %s\n", argv[2], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "mode")) {
        if (argc < 3) {
            printf("  usage: lcd mode <0-3>\n");
            return 1;
        }
        esp_err_t err = ft_lcd_set_spi((uint8_t)atoi(argv[2]), lcd->spi_hz / 1000000);
        printf("  lcd spi mode %s: %s\n", argv[2], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "speed")) {
        if (argc < 3) {
            printf("  usage: lcd speed <MHz>\n");
            return 1;
        }
        esp_err_t err = ft_lcd_set_spi(lcd->spi_mode, (uint32_t)atoi(argv[2]));
        printf("  lcd spi %s MHz: %s\n", argv[2], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "bright")) {
        if (argc < 3) {
            printf("  usage: lcd bright <0-100>\n");
            return 1;
        }
        esp_err_t err = ft_lcd_backlight((uint8_t)atoi(argv[2]));
        printf("  backlight %s%%: %s\n", argv[2], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "on")) {
        ft_lcd_backlight(lcd->brightness ? lcd->brightness : 70);
        return 0;
    }
    if (!strcasecmp(sub, "off")) {
        ft_lcd_backlight(0);
        return 0;
    }
    if (!strcasecmp(sub, "fill")) {
        uint16_t color = parse_color((argc > 2) ? argv[2] : "white");
        esp_err_t err = ft_lcd_fill(color);
        printf("  lcd fill 0x%04X: %s\n", color, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "bars")) {
        ft_lcd_show_bars();
        return 0;
    }
    if (!strcasecmp(sub, "grad")) {
        ft_lcd_show_gradient();
        return 0;
    }
    if (!strcasecmp(sub, "grid")) {
        ft_lcd_show_grid();
        return 0;
    }
    if (!strcasecmp(sub, "pattern")) {
        ft_lcd_show_pattern();
        return 0;
    }
    if (!strcasecmp(sub, "text")) {
        if (argc < 3) {
            printf("  usage: lcd text <string>\n");
            return 1;
        }
        ft_lcd_fill_rect(0, 0, lcd->width, lcd->height, 0x0000);
        ft_lcd_draw_text(10, 10, argv[2], 0xFFFF, 0x0000, 2);
        ft_lcd_flush();
        return 0;
    }
    printf("  usage: lcd init|info|res|rot|mode|speed|bright|on|off|fill|bars|grad|grid|pattern|text\n");
    return 1;
}


static void touch_draw_point(uint16_t x, uint16_t y)
{
    const ft_lcd_info_t *lcd = ft_lcd_info();
    if (!lcd->have_fb) {
        return;
    }
    if (x >= lcd->width) {
        x = lcd->width - 1;
    }
    if (y >= lcd->height) {
        y = lcd->height - 1;
    }
    for (int d = -4; d <= 4; d++) {
        ft_lcd_draw_pixel(x + d, y, 0xF800);
        ft_lcd_draw_pixel(x, y + d, 0xF800);
    }
    ft_lcd_flush_region((x > 8) ? (uint16_t)(x - 8) : 0,
                        (y > 8) ? (uint16_t)(y - 8) : 0, 16, 16);
}

static void touch_trace_run(int seconds)
{
    const ft_lcd_info_t *lcd = ft_lcd_info();
    if (!lcd->have_fb) {
        printf("  no framebuffer\n");
        return;
    }
    if (seconds <= 0) {
        seconds = 10;
    }
    if (seconds > 60) {
        seconds = 60;
    }

    ft_lcd_show_grid();
    printf("  touch trace %d s, draw lines on LCD\n", seconds);
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    bool down = false;
    uint16_t lx = 0;
    uint16_t ly = 0;

    while (esp_timer_get_time() < end) {
        ft_touch_data_t d = {0};
        if (ft_touch_read(&d) == ESP_OK && d.points > 0) {
            uint16_t x = d.x;
            uint16_t y = d.y;
            if (x >= lcd->width) x = lcd->width - 1;
            if (y >= lcd->height) y = lcd->height - 1;
            if (down) {
                ft_lcd_draw_line(lx, ly, x, y, 0xF800);
                int x0 = (lx < x ? lx : x) - 2;
                int y0 = (ly < y ? ly : y) - 2;
                int x1 = (lx > x ? lx : x) + 2;
                int y1 = (ly > y ? ly : y) + 2;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 >= lcd->width) x1 = lcd->width - 1;
                if (y1 >= lcd->height) y1 = lcd->height - 1;
                ft_lcd_flush_region((uint16_t)x0, (uint16_t)y0,
                                    (uint16_t)(x1 - x0 + 1), (uint16_t)(y1 - y0 + 1));
            } else {
                touch_draw_point(x, y);
            }
            lx = x;
            ly = y;
            down = true;
        } else {
            down = false;
        }
        ft_delay_ms(15);
    }
}

static int cmd_touch(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "id";
    if (!ft_touch_present()) {
        printf("  CST816D not found\n");
        return 1;
    }
    if (!strcasecmp(sub, "id")) {
        printf("  CST816D id=0x%02X (expect 0xB6) fw=0x%02X INT=%s\n",
               ft_touch_chip_id(), ft_touch_fw_version(), ft_touch_int_level() ? "high" : "low");
        return 0;
    }
    if (!strcasecmp(sub, "read")) {
        ft_touch_data_t d = {0};
        esp_err_t err = ft_touch_read(&d);
        if (err != ESP_OK) {
            printf("  read failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("  status=0x%02X gesture=0x%02X points=%u x=%u y=%u\n",
               d.status, d.gesture, d.points, d.x, d.y);
        return 0;
    }
    if (!strcasecmp(sub, "int")) {
        int last = ft_touch_int_level();
        printf("  INT initial=%d, watching 5 s...\n", last);
        int64_t end = esp_timer_get_time() + 5000000;
        int events = 0;
        while (esp_timer_get_time() < end) {
            int now = ft_touch_int_level();
            if (now != last) {
                printf("  INT -> %d\n", now);
                last = now;
                events++;
            }
            ft_delay_ms(10);
        }
        printf("  INT events: %d\n", events);
        return 0;
    }
    if (!strcasecmp(sub, "trace")) {
        int sec = (argc > 2) ? atoi(argv[2]) : 10;
        touch_trace_run(sec);
        return 0;
    }
    if (!strcasecmp(sub, "stream")) {
        int n = (argc > 2) ? atoi(argv[2]) : 100;
        if (n <= 0) {
            n = 100;
        }
        ft_lcd_show_grid();
        printf("  touch stream %d samples, draw on LCD\n", n);
        for (int i = 0; i < n; i++) {
            ft_touch_data_t d = {0};
            if (ft_touch_read(&d) == ESP_OK && d.points > 0) {
                printf("  x=%u y=%u gesture=0x%02X\n", d.x, d.y, d.gesture);
                touch_draw_point(d.x, d.y);
            }
            ft_delay_ms(20);
        }
        return 0;
    }
    printf("  usage: touch id|read|stream [n]|trace [sec]|int\n");
    return 1;
}

static int cmd_keys(int argc, char **argv)
{
    if (argc > 1 && !strcasecmp(argv[1], "stream")) {
        int sec = (argc > 2) ? atoi(argv[2]) : 10;
        ft_keys_stream(sec);
        return 0;
    }
    ft_keys_print();
    return 0;
}

static int cmd_audio(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "mic";
    int sec = (argc > 2) ? atoi(argv[2]) : 0;
    if (!strcasecmp(sub, "mic")) {
        printf("  audio mic %d s\n", sec ? sec : 5);
        return ft_audio_mic(sec) == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "tone")) {
        printf("  audio tone %d s (1 kHz)\n", sec ? sec : 3);
        return ft_audio_tone(sec) == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "echo")) {
        int gain = (argc > 3) ? atoi(argv[3]) : CONFIG_FT_ECHO_GAIN_X10;
        printf("  audio echo %d s gain x%.1f\n", sec ? sec : 10, gain / 10.0f);
        return ft_audio_echo(sec, gain) == ESP_OK ? 0 : 1;
    }
    printf("  usage: audio mic|tone|echo [sec] [gain]\n");
    return 1;
}

static int cmd_wifi(int argc, char **argv)
{
    const char *sub = (argc > 1) ? argv[1] : "scan";
    if (!strcasecmp(sub, "scan")) {
        int n = (argc > 2) ? atoi(argv[2]) : 10;
        esp_err_t err = ft_wifi_scan(n);
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "connect")) {
        if (argc < 3) {
            printf("  usage: wifi connect <ssid> [password]\n");
            return 1;
        }
        const char *pass = (argc > 3) ? argv[3] : "";
        esp_err_t err = ft_wifi_connect(argv[2], pass, 15000);
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcasecmp(sub, "status")) {
        ft_wifi_status();
        return 0;
    }
    printf("  usage: wifi scan [n]|connect <ssid> [password]|status\n");
    return 1;
}

static int cmd_bt(int argc, char **argv)
{
    if (argc > 1 && strcasecmp(argv[1], "scan")) {
        printf("  usage: bt scan [sec]\n");
        return 1;
    }
    int sec = (argc > 2) ? atoi(argv[2]) : 5;
    esp_err_t err = ft_bt_scan(sec);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_flash(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ft_flash_print_info();
    return 0;
}

static int cmd_usb(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ft_usb_print_info();
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("  rebooting...\n");
    fflush(stdout);
    ft_delay_ms(100);
    esp_restart();
    return 0;
}

static int cmd_poweroff(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("  AXP2101 shutdown...\n");
    fflush(stdout);
    ft_pmic_poweroff();
    return 0;
}


static int cli_dispatch(int argc, char **argv)
{
    if (argc < 1 || !argv[0]) {
        return 1;
    }
    for (size_t i = 0; i < s_cmds_len; i++) {
        if (!strcasecmp(argv[0], s_cmds[i].name)) {
            return s_cmds[i].fn(argc, argv);
        }
    }
    printf("  unknown command: %s (type 'help')\n", argv[0]);
    return 1;
}

static void plain_cli_task(void *arg)
{
    (void)arg;
    char line[160];
    printf("  fallback CLI ready. Type 'help'.\n");
    while (1) {
        printf("factorytest> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            ft_delay_ms(100);
            continue;
        }
        char *argv[8] = {0};
        int argc = 0;
        char *save = NULL;
        char *tok = strtok_r(line, " \t\r\n", &save);
        while (tok && argc < 8) {
            argv[argc++] = tok;
            tok = strtok_r(NULL, " \t\r\n", &save);
        }
        if (argc) {
            cli_dispatch(argc, argv);
        }
    }
}

static esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "factorytest>";
    repl_cfg.max_cmdline_length = 160;
    repl_cfg.task_stack_size = 8192;   /* audio echo 使用约 4 KB 栈缓冲 */
    esp_err_t err;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dev_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_console_new_repl_usb_serial_jtag failed: %s", esp_err_to_name(err));
        return err;
    }
#else
    esp_console_dev_uart_config_t dev_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&dev_cfg, &repl_cfg, &repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_console_new_repl_uart failed: %s", esp_err_to_name(err));
        return err;
    }
#endif
    for (size_t i = 0; i < s_cmds_len; i++) {
        esp_console_cmd_t cmd = {
            .command = s_cmds[i].name,
            .help = s_cmds[i].help,
            .hint = NULL,
            .func = s_cmds[i].fn,
        };
        err = esp_console_cmd_register(&cmd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "register '%s' failed: %s", s_cmds[i].name, esp_err_to_name(err));
        }
    }
    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_console_start_repl failed: %s", esp_err_to_name(err));
    }
    return err;
}

void factory_test_start(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    esp_log_level_set("*", ESP_LOG_WARN);

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    memset(&g_ft_present, 0, sizeof(g_ft_present));
    g_ft_present.i2c = (ft_i2c_init() == ESP_OK);
    g_ft_present.pmic = (ft_pmic_init() == ESP_OK);
    g_ft_present.lcd = (ft_lcd_init() == ESP_OK);
    g_ft_present.touch = (ft_touch_init() == ESP_OK);
    g_ft_present.imu = (ft_sensors_init() == ESP_OK);
    g_ft_present.keys = (ft_keys_init() == ESP_OK);

    print_banner();

    if (console_start() != ESP_OK) {
        printf("\n[WARN] esp_console REPL unavailable, starting plain CLI\n");
        xTaskCreate(plain_cli_task, "ft_cli", 8192, NULL, 5, NULL);
    }
}

