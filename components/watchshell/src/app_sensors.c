#include "watch_apps.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "watch_gfx.h"
#include "watch_net.h"
#include "watch_sensors.h"
#include "watch_storage.h"

static int s_page;
static volatile bool s_calibrating;

static const char *s_tabs[] = {"MOTION", "COMPASS", "ENV", "SYSTEM"};

static void sensor_enter(void)
{
    s_page = 0;
}

static void cal_task(void *arg)
{
    (void)arg;
    watch_sensors_calibrate_mag(8);
    s_calibrating = false;
    vTaskDelete(NULL);
}

static void sensor_event(const ws_event_t *ev)
{
    if (ev->type == WS_EV_KEY_DOWN) {
        if (ev->key == WS_KEY_LEFT) s_page = (s_page + 3) % 4;
        else if (ev->key == WS_KEY_RIGHT) s_page = (s_page + 1) % 4;
        return;
    }
    if (ev->type != WS_EV_TOUCH_UP) return;
    if (ev->y >= 24 && ev->y <= 38) {
        int tab = (ev->x - WS_SAFE) / 56;
        if (tab < 0) tab = 0;
        if (tab > 3) tab = 3;
        s_page = tab;
        return;
    }
    if (s_page == 1 && ev->y > 240 && !s_calibrating) {
        s_calibrating = true;
        xTaskCreatePinnedToCore(cal_task, "magcal", 4096, NULL, 3, NULL, 0);
    }
}

static void tabs(void)
{
    for (int i = 0; i < 4; i++) {
        int x = WS_SAFE + i * 56;
        if (i == s_page) {
            watch_gfx_fill_rect(x, 24, 54, 13, WS_COLOR_GREEN);
            watch_gfx_text_center(x + 27, 27, s_tabs[i], WS_COLOR_BLACK, 1);
        } else {
            watch_gfx_rect(x, 24, 54, 13, WS_COLOR_DIM);
            watch_gfx_text_center(x + 27, 27, s_tabs[i], WS_COLOR_GRAY, 1);
        }
    }
}

static void bar(int x, int y, int w, float value, float maxv, uint16_t color)
{
    watch_gfx_rect(x, y, w, 6, WS_COLOR_DIM);
    float f = fabsf(value) / maxv;
    if (f > 1) f = 1;
    watch_gfx_fill_rect(x + 1, y + 1, (int)((w - 2) * f), 4, color);
}

static void render_motion(void)
{
    const ws_imu_t *imu = watch_sensors_imu();
    watch_gfx_text(WS_SAFE, 46, "ICM-42670-P", WS_COLOR_CYAN, 1);
    if (!imu->ok) {
        watch_gfx_text(WS_SAFE, 62, "IMU OFFLINE", WS_COLOR_RED, 2);
        return;
    }
    watch_gfx_printf(WS_SAFE, 62, WS_COLOR_GREEN, 1, "AX %+6.2f g", imu->ax);
    watch_gfx_printf(WS_SAFE, 74, WS_COLOR_GREEN, 1, "AY %+6.2f g", imu->ay);
    watch_gfx_printf(WS_SAFE, 86, WS_COLOR_GREEN, 1, "AZ %+6.2f g", imu->az);
    bar(118, 62, 112, imu->ax, 2.0f, WS_COLOR_GREEN);
    bar(118, 74, 112, imu->ay, 2.0f, WS_COLOR_GREEN);
    bar(118, 86, 112, imu->az, 2.0f, WS_COLOR_GREEN);
    watch_gfx_printf(WS_SAFE, 106, WS_COLOR_CYAN, 1, "GX %+7.1f dps", imu->gx);
    watch_gfx_printf(WS_SAFE, 118, WS_COLOR_CYAN, 1, "GY %+7.1f dps", imu->gy);
    watch_gfx_printf(WS_SAFE, 130, WS_COLOR_CYAN, 1, "GZ %+7.1f dps", imu->gz);
    bar(118, 106, 112, imu->gx, 500.0f, WS_COLOR_CYAN);
    bar(118, 118, 112, imu->gy, 500.0f, WS_COLOR_CYAN);
    bar(118, 130, 112, imu->gz, 500.0f, WS_COLOR_CYAN);
    watch_gfx_printf(WS_SAFE, 154, WS_COLOR_AMBER, 1, "TEMP  %.1f C", imu->temp_c);
    watch_gfx_printf(WS_SAFE, 166, WS_COLOR_AMBER, 1, "STEPS %d", imu->steps);
    float pitch = atan2f(-imu->ax, sqrtf(imu->ay * imu->ay + imu->az * imu->az)) * 57.2958f;
    float roll = atan2f(imu->ay, imu->az) * 57.2958f;
    watch_gfx_printf(WS_SAFE, 186, WS_COLOR_GRAY, 1, "PITCH %+4.1f ROLL %+4.1f", pitch, roll);
    watch_gfx_text(WS_SAFE, 210, "LEFT/RIGHT: page  PWR: back", WS_COLOR_DIM, 1);
}

static void render_compass(void)
{
    const ws_mag_t *mag = watch_sensors_mag();
    int cx = 120, cy = 142;
    watch_gfx_circle(cx, cy, 72, WS_COLOR_DIM);
    watch_gfx_circle(cx, cy, 66, 0x01A0);
    for (int i = 0; i < 8; i++) {
        float a = (i * 45 - 90) * 3.14159265f / 180.0f;
        int x0 = cx + (int)(cosf(a) * 58);
        int y0 = cy + (int)(sinf(a) * 58);
        int x1 = cx + (int)(cosf(a) * 66);
        int y1 = cy + (int)(sinf(a) * 66);
        watch_gfx_line(x0, y0, x1, y1, WS_COLOR_GREEN);
    }
    if (mag->ok) {
        float a = (mag->heading_deg - 90) * 3.14159265f / 180.0f;
        int nx = cx + (int)(cosf(a) * 55);
        int ny = cy + (int)(sinf(a) * 55);
        watch_gfx_line(cx, cy, nx, ny, WS_COLOR_RED);
        watch_gfx_fill_circle(nx, ny, 4, WS_COLOR_RED);
        const char *dir = "N";
        int h = (int)mag->heading_deg;
        if (h >= 45 && h < 135) dir = "E";
        else if (h >= 135 && h < 225) dir = "S";
        else if (h >= 225 && h < 315) dir = "W";
        watch_gfx_text_center(cx, cy - 14, dir, WS_COLOR_WHITE, 2);
        watch_gfx_printf(WS_SAFE, 224, WS_COLOR_GREEN, 1, "HDG %5.1f deg RAW %d %d %d",
                         mag->heading_deg, mag->x, mag->y, mag->z);
        watch_gfx_printf(WS_SAFE, 236, WS_COLOR_GRAY, 1, "field age %u ms", (unsigned)mag->age_ms);
    } else {
        watch_gfx_text_center(cx, cy, "NO MAG", WS_COLOR_RED, 2);
    }
    watch_gfx_rect(176, 246, 56, 18, s_calibrating ? WS_COLOR_AMBER : WS_COLOR_CYAN);
    watch_gfx_text_center(204, 251, s_calibrating ? "WAIT" : "CAL 8s",
                          s_calibrating ? WS_COLOR_AMBER : WS_COLOR_CYAN, 1);
}

static void render_env(void)
{
    const ws_env_t *env = watch_sensors_env();
    watch_gfx_text(WS_SAFE, 46, "AHT20", WS_COLOR_CYAN, 1);
    if (!env->ok) {
        watch_gfx_text(WS_SAFE, 70, "ENV OFFLINE", WS_COLOR_RED, 2);
        return;
    }
    watch_gfx_printf(WS_SAFE, 66, WS_COLOR_WHITE, 4, "%.1f", env->temp_c);
    watch_gfx_text(WS_SAFE, 100, "C", WS_COLOR_GREEN, 2);
    watch_gfx_printf(WS_SAFE, 126, WS_COLOR_WHITE, 4, "%.0f", env->rh);
    watch_gfx_text(WS_SAFE, 160, "%RH", WS_COLOR_GREEN, 2);
    watch_gfx_text(WS_SAFE, 200, "TEMP", WS_COLOR_GRAY, 1);
    watch_gfx_text(120, 200, "HUMIDITY", WS_COLOR_GRAY, 1);
    watch_gfx_text(WS_SAFE, 220, "AHT20 sample every 2 s", WS_COLOR_DIM, 1);
    watch_gfx_text(WS_SAFE, 232, "upright, away from breath", WS_COLOR_DIM, 1);
}

static void render_system(void)
{
    ws_power_t p = watch_sensors_power();
    ws_net_status_t n;
    watch_net_status(&n);
    watch_gfx_text(WS_SAFE, 46, "POWER / SYSTEM", WS_COLOR_CYAN, 1);
    watch_gfx_printf(WS_SAFE, 62, WS_COLOR_GREEN, 1, "BATTERY %3d%%  %4d mV", p.batt_pct, p.batt_mv);
    watch_gfx_printf(WS_SAFE, 74, WS_COLOR_GREEN, 1, "VSYS    %4d mV VBUS %4d mV", p.vsys_mv, p.vbus_mv);
    watch_gfx_printf(WS_SAFE, 86, WS_COLOR_GREEN, 1, "CHARGE  %s", p.charging ? "USB" : "IDLE");
    watch_gfx_printf(WS_SAFE, 104, WS_COLOR_CYAN, 1, "WIFI    %s  %d dBm", n.connected ? "UP" : "DOWN", n.rssi);
    watch_gfx_printf(WS_SAFE, 116, WS_COLOR_CYAN, 1, "IP      %s", n.ip);
    watch_gfx_printf(WS_SAFE, 128, WS_COLOR_CYAN, 1, "AP      %s", n.ap_mode ? "ON" : "OFF");
    watch_gfx_printf(WS_SAFE, 146, WS_COLOR_AMBER, 1, "HEAP    %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    watch_gfx_printf(WS_SAFE, 158, WS_COLOR_AMBER, 1, "PSRAM   %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    watch_gfx_printf(WS_SAFE, 170, WS_COLOR_AMBER, 1, "UPTIME  %lld s", esp_timer_get_time() / 1000000);
    watch_gfx_printf(WS_SAFE, 188, WS_COLOR_GRAY, 1, "STORAGE %s", watch_storage_ready() ? "MOUNTED" : "FAILED");
    watch_gfx_printf(WS_SAFE, 200, WS_COLOR_GRAY, 1, "FAT     /storage  16 MB flash");
    watch_gfx_text(WS_SAFE, 226, "sensors: IMU / MAG / AHT20 / RTC", WS_COLOR_DIM, 1);
}

static void sensor_render(void)
{
    tabs();
    switch (s_page) {
    case 0: render_motion(); break;
    case 1: render_compass(); break;
    case 2: render_env(); break;
    default: render_system(); break;
    }
}

const ws_app_impl_t ws_app_sensors = {
    .id = "sensors",
    .name = "SENSORS",
    .subtitle = "IMU / compass / AHT20",
    .enter = sensor_enter,
    .leave = NULL,
    .render = sensor_render,
    .event = sensor_event,
    .tick = NULL,
};
