#include "watch_apps.h"

#include <math.h>
#include <stdio.h>

#include "esp_timer.h"
#include "watch_gfx.h"
#include "watch_sensors.h"
#include "watch_theme.h"

static int s_egg_taps;
static int64_t s_egg_first;

static void wf_enter(void) {}

static void wf_render(void)
{
    const ws_time_t *t = watch_sensors_time();
    const ws_env_t *env = watch_sensors_env();
    const ws_imu_t *imu = watch_sensors_imu();
    const ws_mag_t *mag = watch_sensors_mag();
    ws_power_t p = watch_sensors_power();

    int hour = 0, minute = 0, second = 0;
    if (t->valid) {
        hour = t->hour; minute = t->minute; second = t->second;
    } else {
        int sec = (int)(esp_timer_get_time() / 1000000);
        hour = (sec / 3600) % 24; minute = (sec / 60) % 60; second = sec % 60;
    }

    watch_gfx_scanlines(WS_UI_TOP, WS_SCREEN_H - WS_UI_TOP, 0x0120);
    watch_gfx_circle(120, 112, 78, WS_COLOR_DIM);
    watch_gfx_circle(120, 112, 74, 0x01A0);
    for (int i = 0; i < 12; i++) {
        float a = (i * 30 - 90) * 3.14159265f / 180.0f;
        int x0 = 120 + (int)(cosf(a) * 66);
        int y0 = 112 + (int)(sinf(a) * 66);
        int x1 = 120 + (int)(cosf(a) * 72);
        int y1 = 112 + (int)(sinf(a) * 72);
        watch_gfx_line(x0, y0, x1, y1, WS_COLOR_GREEN);
    }
    float ha = ((hour % 12) * 30 + minute * 0.5f - 90) * 3.14159265f / 180.0f;
    float ma = (minute * 6 - 90) * 3.14159265f / 180.0f;
    float sa = (second * 6 - 90) * 3.14159265f / 180.0f;
    watch_gfx_line(120, 112, 120 + (int)(cosf(ha) * 38), 112 + (int)(sinf(ha) * 38), WS_COLOR_WHITE);
    watch_gfx_line(120, 112, 120 + (int)(cosf(ma) * 56), 112 + (int)(sinf(ma) * 56), WS_COLOR_GREEN);
    watch_gfx_line(120, 112, 120 + (int)(cosf(sa) * 62), 112 + (int)(sinf(sa) * 62), WS_COLOR_RED);
    watch_gfx_fill_circle(120, 112, 3, WS_COLOR_GREEN);

    char big[8];
    snprintf(big, sizeof(big), "%02d:%02d", hour, minute);
    watch_gfx_text_center(120, 200, big, WS_COLOR_WHITE, 4);
    char date[24];
    if (t->valid) snprintf(date, sizeof(date), "%04d.%02d.%02d", t->year, t->month, t->day);
    else snprintf(date, sizeof(date), "RTC OFFLINE");
    watch_gfx_text_center(120, 232, date, WS_COLOR_GREEN, 1);

    char line[40];
    snprintf(line, sizeof(line), "BATT %3d%%  %4d mV", p.batt_pct, p.batt_mv);
    watch_gfx_text(WS_SAFE + 2, 246, line, WS_COLOR_GREEN, 1);
    if (env->ok) snprintf(line, sizeof(line), "ENV  %.1fC  %.0f%%RH", env->temp_c, env->rh);
    else snprintf(line, sizeof(line), "ENV  --");
    watch_gfx_text(WS_SAFE + 2, 256, line, WS_COLOR_CYAN, 1);
    snprintf(line, sizeof(line), "STEP %d  HDG %.0f", imu->steps, mag->ok ? mag->heading_deg : 0.0f);
    watch_gfx_text(WS_SAFE + 2, 266, line, WS_COLOR_GRAY, 1);
}

static void wf_tick(void) {}

static void wf_event(const ws_event_t *ev)
{
    if (!ev || ev->type != WS_EV_TOUCH_UP) return;
    bool hit = ev->x >= 40 && ev->x <= 200 && ev->y >= 226 && ev->y <= 242;
    if (!hit) {
        s_egg_taps = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_egg_taps == 0 || now - s_egg_first > 3000000) {
        s_egg_taps = 0;
        s_egg_first = now;
    }
    s_egg_taps++;
    if (s_egg_taps >= 7) {
        s_egg_taps = 0;
        watch_theme_unlock_error();
    }
}

const ws_app_impl_t ws_app_watchface = {
    .id = "watchface",
    .name = "WATCHFACE",
    .subtitle = "time / power / motion",
    .enter = wf_enter,
    .leave = NULL,
    .render = wf_render,
    .event = wf_event,
    .tick = wf_tick,
};
