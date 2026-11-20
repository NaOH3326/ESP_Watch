#include "watch_apps.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "watch_gfx.h"
#include "watch_script.h"
#include "watch_storage.h"
#include "watch_term.h"

#define SHELL_LIST_ROWS 4
#define SHELL_OUT_Y 82
#define SHELL_OUT_LINES 18
#define SHELL_BTN_Y 248
#define SHELL_BTN_H 24
#define SHELL_BTN_W 72
#define SHELL_LIST_X WS_SAFE
#define SHELL_LIST_W (WS_SCREEN_W - 2 * SHELL_LIST_X)

static char s_scripts[32][48];
static int s_count;
static int s_sel;
static int s_list_top;
static bool s_was_busy;

static void shell_refresh(void)
{
    s_count = watch_storage_list_scripts(s_scripts, 32);
    if (s_sel >= s_count) s_sel = s_count ? s_count - 1 : 0;
}

static void shell_enter(void)
{
    shell_refresh();
    s_sel = 0;
    s_list_top = 0;
    s_was_busy = false;
    watch_term_set_prompt("root@esp-watch:~#");
    if (watch_term_line_count() == 0) {
        watch_term_printf(WS_COLOR_GREEN, "WatchShell 0.1 / ESP32-S3\n");
        watch_term_printf(WS_COLOR_DIM, "scripts loaded: %d\n", s_count);
        watch_term_printf(WS_COLOR_GRAY, "UP/DOWN select, EXEC run, PWR back\n");
    }
}

static void shell_leave(void)
{
    if (watch_script_busy()) watch_script_stop();
}

static void shell_run(void)
{
    if (watch_script_busy()) {
        watch_script_stop();
        return;
    }
    if (!s_count) {
        watch_term_printf(WS_COLOR_RED, "no scripts in /storage/scripts\n");
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", WS_SCRIPT_DIR, s_scripts[s_sel]);
    if (!watch_script_execute(path)) {
        watch_term_printf(WS_COLOR_RED, "cannot start script\n");
    }
}

static void shell_select(int delta)
{
    if (!s_count) return;
    s_sel += delta;
    if (s_sel < 0) s_sel = 0;
    if (s_sel >= s_count) s_sel = s_count - 1;
    if (s_sel < s_list_top) s_list_top = s_sel;
    if (s_sel >= s_list_top + SHELL_LIST_ROWS) s_list_top = s_sel - SHELL_LIST_ROWS + 1;
}

static void shell_event(const ws_event_t *ev)
{
    if (ev->type == WS_EV_KEY_DOWN) {
        if (ev->key == WS_KEY_LEFT) shell_select(-1);
        else if (ev->key == WS_KEY_RIGHT) shell_select(1);
        return;
    }
    if (ev->type == WS_EV_TOUCH_MOVE) {
        if (ev->y > SHELL_OUT_Y && ev->y < SHELL_BTN_Y) {
            static int accum;
            accum += ev->dy;
            if (accum > 6) { watch_term_scroll_by(1); accum = 0; }
            if (accum < -6) { watch_term_scroll_by(-1); accum = 0; }
        }
        return;
    }
    if (ev->type != WS_EV_TOUCH_UP) return;
    if (ev->y >= SHELL_BTN_Y && ev->y <= SHELL_BTN_Y + SHELL_BTN_H) {
        if (ev->x < 82) shell_select(-1);
        else if (ev->x < 158) shell_select(1);
        else shell_run();
        return;
    }
    if (ev->y >= 34 && ev->y < 34 + SHELL_LIST_ROWS * 10) {
        int row = (ev->y - 34) / 10;
        int idx = s_list_top + row;
        if (idx >= 0 && idx < s_count) s_sel = idx;
        return;
    }
    if (ev->y > SHELL_OUT_Y && ev->y < SHELL_BTN_Y) {
        watch_term_follow();
    }
}

static void button(int x, const char *label, uint16_t color)
{
    watch_gfx_rect(x, SHELL_BTN_Y, SHELL_BTN_W, SHELL_BTN_H, color);
    watch_gfx_text_center(x + SHELL_BTN_W / 2, SHELL_BTN_Y + 8, label, color, 1);
}

static void shell_render(void)
{
    char head[48];
    snprintf(head, sizeof(head), "WATCHSHELL  %02d/%02d", s_count ? s_sel + 1 : 0, s_count);
    watch_gfx_text(WS_SAFE, 25, head, WS_COLOR_GREEN, 1);
    watch_gfx_text_right(WS_SCREEN_W - WS_SAFE, 25, watch_script_busy() ? "RUN" : "IDLE",
                         watch_script_busy() ? WS_COLOR_AMBER : WS_COLOR_DIM, 1);
    watch_gfx_hline(WS_SAFE, 33, WS_SCREEN_W - 2 * WS_SAFE, WS_COLOR_DIM);

    for (int row = 0; row < SHELL_LIST_ROWS; row++) {
        int idx = s_list_top + row;
        int y = 36 + row * 10;
        if (idx >= s_count) break;
        bool sel = idx == s_sel;
        if (sel) watch_gfx_fill_rect(SHELL_LIST_X, y - 1, SHELL_LIST_W, 9, 0x02A0);
        watch_gfx_printf(WS_SAFE, y, sel ? WS_COLOR_BLACK : WS_COLOR_GRAY, 1,
                         "%c %s", sel ? '>' : ' ', s_scripts[idx]);
    }
    watch_gfx_hline(WS_SAFE, 77, WS_SCREEN_W - 2 * WS_SAFE, WS_COLOR_DIM);

    watch_term_render(WS_SAFE, SHELL_OUT_Y, SHELL_OUT_LINES, WS_COLOR_GREEN, false);

    const char *prompt = watch_term_prompt();
    watch_gfx_text(WS_SAFE, 230, prompt, WS_COLOR_GREEN, 1);
    int px = WS_SAFE + watch_gfx_text_width(prompt, 1) + 4;
    if (!watch_script_busy() && ((esp_timer_get_time() / 400000) & 1)) {
        watch_gfx_text(px, 230, "_", WS_COLOR_GREEN, 1);
    } else if (watch_script_busy()) {
        static const char spin[] = "|/-\\";
        watch_gfx_text(px, 230, (char[]){ spin[(esp_timer_get_time() / 120000) & 3], 0 },
                       WS_COLOR_AMBER, 1);
        watch_gfx_text(WS_SAFE, 240, watch_script_current(), WS_COLOR_AMBER, 1);
    }

    button(WS_SAFE, "UP", WS_COLOR_GREEN);
    button(WS_SAFE + 74, "DOWN", WS_COLOR_GREEN);
    button(WS_SAFE + 148, watch_script_busy() ? "STOP" : "EXEC",
           watch_script_busy() ? WS_COLOR_RED : WS_COLOR_CYAN);
}

static void shell_tick(void)
{
    bool busy = watch_script_busy();
    if (s_was_busy && !busy) {
        shell_refresh();
    }
    s_was_busy = busy;
}

const ws_app_impl_t ws_app_shell = {
    .id = "shell",
    .name = "SHELL",
    .subtitle = "select / execute / scroll",
    .enter = shell_enter,
    .leave = shell_leave,
    .render = shell_render,
    .event = shell_event,
    .tick = shell_tick,
};
