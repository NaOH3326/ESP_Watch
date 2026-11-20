#include "watch_ui.h"
#include "watch_apps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ft_lcd.h"
#include "ft_pmic.h"
#include "watch_audio.h"
#include "watch_cloud.h"
#include "watch_config.h"
#include "watch_gfx.h"
#include "watch_input.h"
#include "watch_net.h"
#include "watch_power.h"
#include "watch_sensors.h"
#include "watch_term.h"
#include "watch_theme.h"

static const char *TAG = "WATCH_UI";
static const ws_app_impl_t *s_apps[WS_APP_COUNT] = {
    &ws_app_watchface, &ws_app_ai, &ws_app_shell,
    &ws_app_sensors, &ws_app_media, &ws_app_games, &ws_app_settings,
};
static int s_active = -1;
static float s_pos = 0;
static int s_target;
static float s_anim_target;
static bool s_dragging;
static int s_down_x;
static int s_down_y;
static int s_last_x;
static int s_pwr_down_ms;
static int s_brightness = 75;
static volatile bool s_render_paused;
static volatile bool s_render_busy;
static SemaphoreHandle_t s_render_lock;
static QueueHandle_t s_inject_q;
static bool s_pause_held;
static bool s_app_fullscreen;
static bool s_app_chrome_hidden;

#define PWR_SHORT_MS       650
#define PWR_MENU_HOLD_MS   1200

#define PM_PANEL_X         16
#define PM_PANEL_Y         44
#define PM_PANEL_W         208
#define PM_PANEL_H         204
#define PM_BTN_X           32
#define PM_BTN_W           176
#define PM_BTN_H           38
#define PM_BTN_Y           96
#define PM_BTN_STEP        46

enum {
    PM_OFF = 0,
    PM_REBOOT,
    PM_CANCEL,
    PM_COUNT,
};

static const char *const s_pm_labels[PM_COUNT] = {
    "POWER OFF", "REBOOT", "CANCEL",
};
static const uint16_t s_pm_colors[PM_COUNT] = {
    WS_COLOR_RED, WS_COLOR_AMBER, WS_COLOR_GREEN,
};
static bool s_power_menu;
static int s_power_menu_sel = PM_CANCEL;
static int s_power_menu_touch = -1;
static bool s_pwr_long_consumed;

static int wrap_index(int i)
{
    i %= WS_APP_COUNT;
    if (i < 0) i += WS_APP_COUNT;
    return i;
}

static int current_index(void)
{
    return wrap_index((int)floorf(s_pos + 0.5f));
}

static const char *app_name(int i)
{
    if (i < 0 || i >= WS_APP_COUNT) return "?";
    return s_apps[i]->name;
}

static void status_bar(void)
{
    watch_gfx_fill_rect(0, 0, WS_SCREEN_W, WS_UI_TOP, WS_COLOR_BLACK);
    watch_gfx_hline(WS_SAFE, WS_UI_TOP - 1, WS_SCREEN_W - 2 * WS_SAFE, WS_COLOR_DIM);

    const ws_time_t *t = watch_sensors_time();
    char clock[8] = "--:--";
    if (t->valid) snprintf(clock, sizeof(clock), "%02d:%02d", t->hour, t->minute);
    else {
        int sec = (int)(esp_timer_get_time() / 1000000);
        snprintf(clock, sizeof(clock), "%02d:%02d", (sec / 60) % 60, sec % 60);
    }
    watch_gfx_text(WS_SAFE + 16, 6, clock, WS_COLOR_GREEN, 1);

    ws_power_t p = watch_sensors_power();
    watch_gfx_battery(168, 5, p.batt_pct, p.charging, WS_COLOR_GREEN);
    watch_gfx_printf(188, 6, WS_COLOR_GREEN, 1, "%3d%%", p.batt_pct);

    if (watch_net_connected()) {
        watch_gfx_wifi(146, 4, watch_net_rssi(), WS_COLOR_GREEN);
    } else if (watch_net_ap_mode()) {
        watch_gfx_wifi(146, 4, -127, WS_COLOR_AMBER);
    }
    ws_ai_state_t ai = watch_cloud_state();
    uint16_t ai_color = ai == WS_AI_ERROR ? WS_COLOR_RED :
                        ai == WS_AI_OFFLINE ? WS_COLOR_DIM : WS_COLOR_CYAN;
    watch_gfx_fill_rect(138, 8, 4, 4, ai_color);
}

void ws_app_wrap_text(int x, int y, int max_chars, const char *text, uint16_t color, int max_lines)
{
    if (!text) return;
    int line = 0, col = 0;
    int cx = x;
    for (const char *p = text; *p && line < max_lines; p++) {
        if (*p == '\n') {
            cx = x; col = 0; line++;
            y += watch_gfx_text_height(1);
            continue;
        }
        if (col >= max_chars) {
            cx = x; col = 0; line++;
            y += watch_gfx_text_height(1);
            if (line >= max_lines) break;
        }
        watch_gfx_char(cx, y, *p, color, 1);
        cx += 6;
        col++;
    }
}

static void power_menu_close(void)
{
    s_power_menu = false;
    s_power_menu_touch = -1;
    s_pwr_long_consumed = false;
    s_pwr_down_ms = 0;
}

static void power_menu_open(bool pwr_still_down)
{
    s_power_menu = true;
    s_power_menu_sel = PM_CANCEL;
    s_power_menu_touch = -1;
    s_pwr_long_consumed = pwr_still_down;
    s_dragging = false;
    watch_power_note_activity();
    ESP_LOGI(TAG, "power menu opened by PWR %s", pwr_still_down ? "hold" : "release");
}

static void power_menu_activate(int item)
{
    if (item < 0 || item >= PM_COUNT) return;
    s_power_menu = false;
    s_power_menu_touch = -1;
    s_pwr_long_consumed = false;
    s_pwr_down_ms = 0;

    if (item == PM_OFF) {
        ESP_LOGI(TAG, "power menu: POWER OFF");
        watch_power_off(WS_POWEROFF_ACTIVE);
    } else if (item == PM_REBOOT) {
        ESP_LOGI(TAG, "power menu: REBOOT");
        esp_restart();
    } else {
        ESP_LOGI(TAG, "power menu: CANCEL");
        watch_power_note_activity();
    }
}

static int power_menu_hit(int x, int y)
{
    if (x < PM_BTN_X || x >= PM_BTN_X + PM_BTN_W) return -1;
    for (int i = 0; i < PM_COUNT; i++) {
        int top = PM_BTN_Y + i * PM_BTN_STEP;
        if (y >= top && y < top + PM_BTN_H) return i;
    }
    return -1;
}

static void power_menu_event(const ws_event_t *ev)
{
    if (ev->type == WS_EV_KEY_DOWN) {
        if (ev->key == WS_KEY_LEFT) {
            s_power_menu_sel = (s_power_menu_sel + PM_COUNT - 1) % PM_COUNT;
        } else if (ev->key == WS_KEY_RIGHT) {
            s_power_menu_sel = (s_power_menu_sel + 1) % PM_COUNT;
        } else if (ev->key == WS_KEY_PWR) {
            s_pwr_down_ms = (int)(esp_timer_get_time() / 1000);
            s_pwr_long_consumed = false;
        }
        return;
    }

    if (ev->type == WS_EV_KEY_UP && ev->key == WS_KEY_PWR) {
        int now = (int)(esp_timer_get_time() / 1000);
        int held = now - s_pwr_down_ms;
        bool consumed = s_pwr_long_consumed;
        s_pwr_long_consumed = false;
        s_pwr_down_ms = 0;
        if (!consumed && held < PWR_SHORT_MS) {
            power_menu_activate(s_power_menu_sel);
        }
        return;
    }

    if (ev->type == WS_EV_TOUCH_DOWN) {
        int item = power_menu_hit(ev->x, ev->y);
        s_power_menu_touch = item;
        if (item >= 0) s_power_menu_sel = item;
        return;
    }

    if (ev->type == WS_EV_TOUCH_UP) {
        int item = power_menu_hit(ev->x, ev->y);
        int pressed = s_power_menu_touch;
        s_power_menu_touch = -1;
        if (item >= 0 && item == pressed) power_menu_activate(item);
    }
}

static void power_menu_tick(void)
{
    if (s_power_menu || s_pwr_down_ms <= 0) return;
    if (!watch_input_key_down(WS_KEY_PWR)) {
        s_pwr_down_ms = 0;
        return;
    }
    int now = (int)(esp_timer_get_time() / 1000);
    if (now - s_pwr_down_ms >= PWR_MENU_HOLD_MS) {
        power_menu_open(true);
    }
}

static void power_menu_draw(void)
{
    watch_gfx_fill_rect(PM_PANEL_X, PM_PANEL_Y, PM_PANEL_W, PM_PANEL_H, WS_COLOR_BLACK);
    watch_gfx_fill_rect(PM_PANEL_X, PM_PANEL_Y, PM_PANEL_W, 3, WS_COLOR_CYAN);
    watch_gfx_rect(PM_PANEL_X, PM_PANEL_Y, PM_PANEL_W, PM_PANEL_H, WS_COLOR_CYAN);

    watch_gfx_text_center(120, 56, "POWER MENU", WS_COLOR_WHITE, 2);
    watch_gfx_text_center(120, 78, "L/R SELECT   PWR RUN", WS_COLOR_GRAY, 1);

    for (int i = 0; i < PM_COUNT; i++) {
        int y = PM_BTN_Y + i * PM_BTN_STEP;
        bool sel = i == s_power_menu_sel;
        watch_gfx_fill_rect(PM_BTN_X, y, PM_BTN_W, PM_BTN_H,
                            sel ? 0x0100 : WS_COLOR_BLACK);
        watch_gfx_rect(PM_BTN_X, y, PM_BTN_W, PM_BTN_H,
                       sel ? WS_COLOR_WHITE : s_pm_colors[i]);

        int scale = 2;
        if (watch_gfx_text_width(s_pm_labels[i], scale) > PM_BTN_W - 8) scale = 1;
        int ty = y + (PM_BTN_H - watch_gfx_text_height(scale)) / 2;
        watch_gfx_text_center(120, ty, s_pm_labels[i],
                              sel ? WS_COLOR_WHITE : s_pm_colors[i], scale);
    }

    watch_gfx_text_center(120, 234, "PWR 6s HARD OFF", WS_COLOR_DIM, 1);
}

static void carousel_background(void)
{
    /* Full-height dot field.  The footer is slightly dimmer so the hint text
     * and page dots stay readable without a hard cut at y=250. */
    for (int y = WS_UI_TOP; y < WS_SCREEN_H; y += 4) {
        uint16_t dot = (y >= 244) ? 0x00A0 : 0x0120;
        for (int x = 0; x < WS_SCREEN_W; x += 3) {
            watch_gfx_pixel(x, y, dot);
        }
    }

    /* Bracketed hairline above the footer: brighter ends, very dim centre. */
    const int divider_y = 242;
    const int end_w = 54;
    watch_gfx_hline(WS_SAFE, divider_y, end_w, 0x01A0);
    watch_gfx_hline(WS_SAFE + end_w, divider_y,
                    WS_SCREEN_W - 2 * WS_SAFE - 2 * end_w, 0x0120);
    watch_gfx_hline(WS_SCREEN_W - WS_SAFE - end_w, divider_y, end_w, 0x01A0);
}

static void carousel_render(void)
{
    watch_theme_tick();
    watch_gfx_clear(WS_COLOR_BLACK);
    const int pivot_x = 120, pivot_y = 430, radius = 300;
    watch_gfx_arc(pivot_x, pivot_y, radius, 255, 285, WS_COLOR_DIM);
    watch_gfx_arc(pivot_x, pivot_y, radius + 5, 258, 282, 0x0120);
    carousel_background();

    /* One icon per screen: the selected app stays centered while the
     * wheel angle only rotates/offsets it slightly during a drag. */
    float nearest = floorf(s_pos + 0.5f);
    int current = wrap_index((int)nearest);
    float off = nearest - s_pos;
    int cx = 120 + (int)(off * 20.0f);
    int cy = 130 + (int)(fabsf(off) * 8.0f);
    int size = 100 - (int)(fabsf(off) * 18.0f);
    watch_gfx_icon((ws_app_id_t)current, cx, cy, size, (int)(off * 50.0f), WS_COLOR_GREEN);
    watch_gfx_text_center(120, 204, s_apps[current]->name, WS_COLOR_WHITE, 2);
    watch_gfx_text_center(120, 224, s_apps[current]->subtitle, WS_COLOR_GRAY, 1);

    watch_gfx_text_center(120, 246, "SWIPE TO ROTATE", WS_COLOR_DIM, 1);
    watch_gfx_text_center(120, 256, "TAP TO OPEN", WS_COLOR_DIM, 1);
    for (int i = 0; i < WS_APP_COUNT; i++) {
        int x = 120 + (i - (WS_APP_COUNT - 1) / 2) * 12;
        if (i == current) watch_gfx_fill_rect(x - 2, 267, 5, 5, WS_COLOR_GREEN);
        else watch_gfx_rect(x, 268, 3, 3, WS_COLOR_DIM);
    }
    status_bar();
    watch_theme_apply();
    watch_theme_draw_overlay();
    if (s_power_menu) power_menu_draw();
    watch_gfx_flush();
}

static void power_warning_render(void)
{
    int mv = watch_power_low_batt_mv();
    int pct = watch_power_low_batt_percent();
    int left = watch_power_low_batt_countdown();
    watch_gfx_clear(WS_COLOR_BLACK);
    watch_gfx_fill_rect(6, 38, WS_SCREEN_W - 12, 190, WS_COLOR_BLACK);
    watch_gfx_rect(6, 38, WS_SCREEN_W - 12, 190, WS_COLOR_RED);
    watch_gfx_fill_rect(6, 38, WS_SCREEN_W - 12, 28, WS_COLOR_RED);
    watch_gfx_text_center(120, 48, "LOW BATTERY", WS_COLOR_BLACK, 2);
    watch_gfx_text_center(120, 84, "CONNECT CHARGER", WS_COLOR_WHITE, 1);
    watch_gfx_printf(58, 108, WS_COLOR_RED, 2, "%d mV", mv);
    watch_gfx_printf(96, 136, WS_COLOR_AMBER, 2, "%d%%", pct);
    watch_gfx_text_center(120, 174, "POWER OFF IN", WS_COLOR_WHITE, 1);
    watch_gfx_printf(108, 192, WS_COLOR_RED, 3, "%d", left > 0 ? left : 0);
    watch_gfx_text_center(120, 246, "PLUG IN USB NOW", WS_COLOR_AMBER, 1);
    watch_gfx_flush();
}

static void app_render(void)
{
    watch_theme_tick();
    watch_gfx_clear(WS_COLOR_BLACK);
    if (s_active >= 0 && s_apps[s_active]->render) {
        s_apps[s_active]->render();
    }
    if (!s_app_fullscreen && !s_app_chrome_hidden) status_bar();
    watch_theme_apply();
    watch_theme_draw_overlay();
    if (s_power_menu) power_menu_draw();
    watch_gfx_flush();
}

static void enter_app(int index)
{
    if (index < 0 || index >= WS_APP_COUNT) return;
    s_app_fullscreen = false;
    s_app_chrome_hidden = false;
    if (s_power_menu) power_menu_close();
    if (s_active >= 0) {
        if (s_apps[s_active]->leave) s_apps[s_active]->leave();
    }
    s_active = index;
    s_pos = (float)index;
    s_target = index;
    s_anim_target = (float)index;
    watch_power_note_activity();
    if (s_apps[index]->enter) s_apps[index]->enter();
}

void watch_ui_open_app(ws_app_id_t app)
{
    enter_app((int)app);
}

void watch_ui_back(void)
{
    s_app_fullscreen = false;
    s_app_chrome_hidden = false;
    if (s_power_menu) power_menu_close();
    if (s_active >= 0) {
        int last = s_active;
        if (s_apps[s_active]->leave) s_apps[s_active]->leave();
        s_active = -1;
        s_pos = (float)last;
        s_target = last;
        s_anim_target = (float)last;
    }
    watch_power_note_activity();
}

bool watch_ui_open_app_by_name(const char *name)
{
    if (!name) return false;
    for (int i = 0; i < WS_APP_COUNT; i++) {
        if (!strcasecmp(name, s_apps[i]->id) || !strcasecmp(name, s_apps[i]->name)) {
            watch_ui_open_app((ws_app_id_t)i);
            return true;
        }
    }
    return false;
}

const char *watch_ui_app_name(ws_app_id_t app)
{
    return app_name((int)app);
}

void watch_ui_pause_render(void)
{
    s_render_paused = true;
    if (s_render_lock) {
        if (!s_pause_held) {
            xSemaphoreTake(s_render_lock, portMAX_DELAY);
            s_pause_held = true;
        }
        return;
    }
    /* Fallback before the UI task creates the lock. */
    for (int i = 0; i < 400 && s_render_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void watch_ui_resume_render(void)
{
    s_render_paused = false;
    if (s_render_lock && s_pause_held) {
        s_pause_held = false;
        xSemaphoreGive(s_render_lock);
    }
}

void watch_ui_set_fullscreen(bool on)
{
    s_app_fullscreen = on;
}

bool watch_ui_fullscreen(void)
{
    return s_app_fullscreen;
}

void watch_ui_set_chrome_hidden(bool on)
{
    s_app_chrome_hidden = on;
}

bool watch_ui_chrome_hidden(void)
{
    return s_app_chrome_hidden;
}

static void set_target(int index)
{
    index = wrap_index(index);
    s_target = index;
    float diff = (float)index - s_pos;
    if (diff > WS_APP_COUNT / 2.0f) diff -= WS_APP_COUNT;
    if (diff < -WS_APP_COUNT / 2.0f) diff += WS_APP_COUNT;
    s_anim_target = s_pos + diff;
}

static void carousel_event(const ws_event_t *ev)
{
    switch (ev->type) {
    case WS_EV_TOUCH_DOWN:
        s_dragging = true;
        s_down_x = ev->x;
        s_down_y = ev->y;
        s_last_x = ev->x;
        break;
    case WS_EV_TOUCH_MOVE:
        if (s_dragging) {
            int dx = ev->x - s_last_x;
            s_last_x = ev->x;
            s_pos -= (float)dx / 210.0f;
            if (s_pos < -0.5f) s_pos += WS_APP_COUNT;
            if (s_pos > WS_APP_COUNT - 0.5f) s_pos -= WS_APP_COUNT;
            s_target = current_index();
            s_anim_target = s_pos;
        }
        break;
    case WS_EV_TOUCH_UP: {
        int dx = ev->x - s_down_x;
        int dy = ev->y - s_down_y;
        s_dragging = false;
        if (abs(dx) + abs(dy) < 16) {
            enter_app(current_index());
        } else {
            set_target(current_index());
        }
        break;
    }
    case WS_EV_KEY_DOWN:
        if (ev->key == WS_KEY_LEFT) {
            set_target(s_target - 1);
        } else if (ev->key == WS_KEY_RIGHT) {
            set_target(s_target + 1);
        } else if (ev->key == WS_KEY_PWR) {
            s_pwr_down_ms = (int)(esp_timer_get_time() / 1000);
        }
        break;
    case WS_EV_KEY_UP:
        if (ev->key == WS_KEY_PWR) {
            int now = (int)(esp_timer_get_time() / 1000);
            bool have_down = s_pwr_down_ms > 0;
            int held = have_down ? now - s_pwr_down_ms : 0;
            s_pwr_down_ms = 0;
            if (!have_down) return;
            if (held >= PWR_MENU_HOLD_MS) {
                power_menu_open(false);
            } else if (held < PWR_SHORT_MS) {
                enter_app(current_index());
            }
        }
        break;
    default:
        break;
    }
}

static void active_event(const ws_event_t *ev)
{
    static bool edge_back;
    if (s_app_fullscreen) {
        if (ev->type == WS_EV_TOUCH_DOWN || ev->type == WS_EV_TOUCH_MOVE ||
            ev->type == WS_EV_TOUCH_UP) {
            if (ev->type == WS_EV_TOUCH_UP) s_app_fullscreen = false;
            return;
        }
        if (ev->type == WS_EV_KEY_DOWN && ev->key == WS_KEY_PWR) {
            s_pwr_down_ms = (int)(esp_timer_get_time() / 1000);
            return;
        }
        if (ev->type == WS_EV_KEY_UP && ev->key == WS_KEY_PWR) {
            s_app_fullscreen = false;
            s_pwr_down_ms = 0;
            return;
        }
        return;
    }
    if (ev->type == WS_EV_TOUCH_DOWN) {
        edge_back = ev->x < 26;
    } else if (ev->type == WS_EV_TOUCH_UP && edge_back && ev->dx > 55) {
        edge_back = false;
        watch_ui_back();
        return;
    } else if (ev->type == WS_EV_TOUCH_UP) {
        edge_back = false;
    }
    if (ev->type == WS_EV_KEY_DOWN && ev->key == WS_KEY_PWR) {
        s_pwr_down_ms = (int)(esp_timer_get_time() / 1000);
        return;
    }
    if (ev->type == WS_EV_KEY_UP && ev->key == WS_KEY_PWR) {
        int now = (int)(esp_timer_get_time() / 1000);
        bool have_down = s_pwr_down_ms > 0;
        int held = have_down ? now - s_pwr_down_ms : 0;
        s_pwr_down_ms = 0;
        if (!have_down) return;
        if (held >= PWR_MENU_HOLD_MS) {
            power_menu_open(false);
            return;
        }
        if (held < PWR_SHORT_MS) {
            /* Settings uses PWR to run the highlighted item; holding past
             * PWR_MENU_HOLD_MS opens the global power menu instead. */
            if (s_active == WS_APP_SETTINGS && s_apps[s_active]->event) {
                s_apps[s_active]->event(ev);
            } else {
                watch_ui_back();
            }
            return;
        }
        if (s_active == WS_APP_SETTINGS) {
            watch_ui_back();
        }
        return;
    }
    if (s_active >= 0 && s_apps[s_active]->event) {
        s_apps[s_active]->event(ev);
    }
}

static void dispatch_event(const ws_event_t *ev)
{
    if (s_power_menu) {
        power_menu_event(ev);
    } else if (s_active < 0) {
        carousel_event(ev);
    } else {
        active_event(ev);
    }
}

bool watch_ui_tap(int x, int y)
{
    if (!s_inject_q) return false;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= WS_SCREEN_W) x = WS_SCREEN_W - 1;
    if (y >= WS_SCREEN_H) y = WS_SCREEN_H - 1;
    ws_event_t down = { .type = WS_EV_TOUCH_DOWN, .x = (int16_t)x, .y = (int16_t)y };
    ws_event_t up = { .type = WS_EV_TOUCH_UP, .x = (int16_t)x, .y = (int16_t)y };
    if (xQueueSend(s_inject_q, &down, 0) != pdTRUE) return false;
    return xQueueSend(s_inject_q, &up, 0) == pdTRUE;
}


static void ui_task(void *arg)
{
    (void)arg;
    watch_term_init();
    int64_t last_render = 0;
    bool first_frame = true;
    while (1) {
        watch_power_poll();
        bool low_batt = watch_power_low_batt_active();
        if (low_batt && s_power_menu) power_menu_close();

        ws_event_t ev;
        while (watch_input_poll(&ev)) {
            if (low_batt) continue;   /* warning is modal; only VBUS clears it */
            watch_power_note_activity();
            dispatch_event(&ev);
        }

        /* Synthetic taps from the serial console (headless UI tests). */
        while (s_inject_q && xQueueReceive(s_inject_q, &ev, 0) == pdTRUE) {
            if (low_batt) continue;
            watch_power_note_activity();
            dispatch_event(&ev);
        }

        if (!low_batt) power_menu_tick();
        if (!low_batt && !s_power_menu && s_active >= 0 && s_apps[s_active]->tick) {
            s_apps[s_active]->tick();
        }

        if (!low_batt && !s_dragging) {
            float diff = s_anim_target - s_pos;
            if (fabsf(diff) < 0.01f) {
                s_pos = s_anim_target;
                while (s_pos < 0) s_pos += WS_APP_COUNT;
                while (s_pos >= WS_APP_COUNT) s_pos -= WS_APP_COUNT;
                s_anim_target = (float)s_target;
            } else {
                s_pos += diff * 0.30f;
            }
        }
        int64_t now = esp_timer_get_time();
        if (!s_render_paused && now - last_render > 30000) {
            last_render = now;
            if (s_render_lock) xSemaphoreTake(s_render_lock, portMAX_DELAY);
            s_render_busy = true;
            if (low_batt) {
                power_warning_render();
            } else if (s_active < 0) {
                carousel_render();
            } else {
                app_render();
            }
            s_render_busy = false;
            if (s_render_lock) xSemaphoreGive(s_render_lock);
            if (first_frame) {
                first_frame = false;
                ESP_LOGI(TAG, "first frame at %lldms", (long long)(now / 1000));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}

void watch_ui_start(void)
{
    s_brightness = watch_config_get_int("bright", CONFIG_WATCH_BRIGHTNESS);
    if (s_brightness < 5) s_brightness = 5;
    if (s_brightness > 100) s_brightness = 100;
    ft_lcd_backlight((uint8_t)s_brightness);
    int volume = watch_config_get_int("volume", CONFIG_WATCH_AUDIO_VOLUME);
    watch_audio_set_volume(volume);
    s_render_lock = xSemaphoreCreateMutex();
    s_inject_q = xQueueCreate(8, sizeof(ws_event_t));
    xTaskCreatePinnedToCore(ui_task, "watch_ui", 12288, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "UI started");
}
