#include "watch_apps.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_system.h"
#include "ft_lcd.h"
#include "watch_audio.h"
#include "watch_config.h"
#include "watch_gfx.h"
#include "watch_net.h"
#include "watch_power.h"
#include "watch_sensors.h"
#include "watch_storage.h"
#include "watch_theme.h"

static int s_brightness = 75;
static int s_sel;

#define BTN_TOP     146
#define BTN_W       108
#define BTN_H       24
#define BTN_STEP_X  112
#define BTN_STEP_Y  26
#define BTN_COUNT   10

static void idle_cycle(void)
{
    static const int options[] = {30, 60, 120, 300, 0};
    int current = watch_power_idle_seconds();
    int next = options[0];
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        if (options[i] == current) {
            next = options[(i + 1) % (sizeof(options) / sizeof(options[0]))];
            break;
        }
    }
    watch_power_set_idle_seconds(next);
}

static void settings_enter(void)
{
    s_brightness = watch_config_get_int("bright", CONFIG_WATCH_BRIGHTNESS);
    if (s_brightness < 5) s_brightness = 5;
    if (s_brightness > 100) s_brightness = 100;
    s_sel = 0;
}

static void settings_activate(int item)
{
    switch (item) {
    case 0:
        watch_net_start_portal();
        break;
    case 1:
        watch_net_restart_sta();
        break;
    case 2:
        watch_theme_next();
        break;
    case 3:
        idle_cycle();
        break;
    case 4:
        if (s_brightness > 10) s_brightness -= 10;
        ft_lcd_backlight((uint8_t)s_brightness);
        watch_config_set_int("bright", s_brightness);
        break;
    case 5:
        if (s_brightness < 100) s_brightness += 10;
        ft_lcd_backlight((uint8_t)s_brightness);
        watch_config_set_int("bright", s_brightness);
        break;
    case 6: {
        int v = watch_audio_volume() - 10;
        if (v < 0) v = 0;
        watch_audio_set_volume(v);
        watch_config_set_int("volume", v);
        break;
    }
    case 7: {
        int v = watch_audio_volume() + 10;
        if (v > 100) v = 100;
        watch_audio_set_volume(v);
        watch_config_set_int("volume", v);
        break;
    }
    case 8:
        watch_power_off(WS_POWEROFF_ACTIVE);
        break;
    case 9:
        esp_restart();
        break;
    default:
        break;
    }
}

static void settings_event(const ws_event_t *ev)
{
    if (ev->type == WS_EV_KEY_DOWN) {
        if (ev->key == WS_KEY_LEFT) {
            s_sel = (s_sel + BTN_COUNT - 1) % BTN_COUNT;
        } else if (ev->key == WS_KEY_RIGHT) {
            s_sel = (s_sel + 1) % BTN_COUNT;
        }
        return;
    }
    if (ev->type == WS_EV_KEY_UP) {
        if (ev->key == WS_KEY_PWR) {
            settings_activate(s_sel);
        }
        return;
    }
    if (ev->type != WS_EV_TOUCH_UP) return;
    if (ev->x < WS_SAFE || ev->x > WS_SAFE + BTN_STEP_X + BTN_W) return;
    if (ev->y < BTN_TOP || ev->y >= BTN_TOP + 5 * BTN_STEP_Y) return;

    int col = ev->x < 120 ? 0 : 1;
    int row = (ev->y - BTN_TOP) / BTN_STEP_Y;
    if (row > 4) return;
    int item = row * 2 + col;
    s_sel = item;
    settings_activate(item);
}

static void sbtn(int x, int y, const char *label, uint16_t color, int index)
{
    bool sel = index == s_sel;
    watch_gfx_fill_rect(x, y, BTN_W, BTN_H, sel ? 0x0100 : WS_COLOR_BLACK);
    watch_gfx_rect(x, y, BTN_W, BTN_H, sel ? WS_COLOR_WHITE : color);

    int scale = 2;
    if (watch_gfx_text_width(label, scale) > BTN_W - 8) scale = 1;
    int ty = y + (BTN_H - watch_gfx_text_height(scale)) / 2;
    watch_gfx_text_center(x + BTN_W / 2, ty, label,
                          sel ? WS_COLOR_WHITE : color, scale);
}

static void settings_render(void)
{
    ws_net_status_t n;
    watch_net_status(&n);
    ws_power_t p = watch_sensors_power();
    char ai[64] = {0};
    watch_config_get_ai(ai, sizeof(ai), NULL, 0);

    char idle_label[16];
    int idle = watch_power_idle_seconds();
    if (idle > 0) snprintf(idle_label, sizeof(idle_label), "IDLE %d", idle);
    else snprintf(idle_label, sizeof(idle_label), "IDLE OFF");

    watch_gfx_text(WS_SAFE, 26, "SETTINGS / SYSTEM", WS_COLOR_CYAN, 1);
    watch_gfx_hline(WS_SAFE, 36, WS_SCREEN_W - 2 * WS_SAFE, WS_COLOR_DIM);
    watch_gfx_printf(WS_SAFE, 42, WS_COLOR_GREEN, 1, "WIFI %s  %s", n.connected ? "UP" : "DOWN", n.ssid);
    watch_gfx_printf(WS_SAFE, 54, WS_COLOR_GREEN, 1, "IP   %s", n.ip);
    watch_gfx_printf(WS_SAFE, 66, WS_COLOR_AMBER, 1, "BATT %d%% %dmV  AP %s",
                     p.batt_pct, p.batt_mv, n.ap_mode ? "ON" : "OFF");
    watch_gfx_printf(WS_SAFE, 78, WS_COLOR_GREEN, 1, "BRIGHT %d%% VOL %d%% %s",
                     s_brightness, watch_audio_volume(),
                     watch_storage_ready() ? "FS" : "NOFS");
    watch_gfx_printf(WS_SAFE, 90, WS_COLOR_GRAY, 1, "THEME %s  EGG %s",
                     watch_theme_name(),
                     watch_theme_error_unlocked() ? "UNLOCKED" : "LOCKED");
    watch_gfx_printf(WS_SAFE, 102, WS_COLOR_GRAY, 1, "AI URL");
    ws_app_wrap_text(WS_SAFE, 114, 36, ai[0] ? ai : "not configured", WS_COLOR_CYAN, 1);

    watch_gfx_text(WS_SAFE, 134, "L/R SEL PWR RUN HOLD 1.2 MENU", WS_COLOR_DIM, 1);

    sbtn(WS_SAFE, BTN_TOP, "PORTAL", WS_COLOR_CYAN, 0);
    sbtn(WS_SAFE + BTN_STEP_X, BTN_TOP, "RECONN", WS_COLOR_GREEN, 1);
    sbtn(WS_SAFE, BTN_TOP + BTN_STEP_Y, "THEME", WS_COLOR_MAGENTA, 2);
    sbtn(WS_SAFE + BTN_STEP_X, BTN_TOP + BTN_STEP_Y, idle_label, WS_COLOR_CYAN, 3);
    sbtn(WS_SAFE, BTN_TOP + 2 * BTN_STEP_Y, "BRIGHT-", WS_COLOR_GREEN, 4);
    sbtn(WS_SAFE + BTN_STEP_X, BTN_TOP + 2 * BTN_STEP_Y, "BRIGHT+", WS_COLOR_GREEN, 5);
    sbtn(WS_SAFE, BTN_TOP + 3 * BTN_STEP_Y, "VOL-", WS_COLOR_GREEN, 6);
    sbtn(WS_SAFE + BTN_STEP_X, BTN_TOP + 3 * BTN_STEP_Y, "VOL+", WS_COLOR_GREEN, 7);
    sbtn(WS_SAFE, BTN_TOP + 4 * BTN_STEP_Y, "PWR OFF", WS_COLOR_RED, 8);
    sbtn(WS_SAFE + BTN_STEP_X, BTN_TOP + 4 * BTN_STEP_Y, "REBOOT", WS_COLOR_RED, 9);
}

const ws_app_impl_t ws_app_settings = {
    .id = "settings",
    .name = "SETTINGS",
    .subtitle = "wifi / portal / display",
    .enter = settings_enter,
    .leave = NULL,
    .render = settings_render,
    .event = settings_event,
    .tick = NULL,
};
