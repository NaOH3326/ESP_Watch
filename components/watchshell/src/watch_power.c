#include "watch_power.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ft_lcd.h"
#include "ft_pmic.h"
#include "sdkconfig.h"
#include "watch_cloud.h"
#include "watch_config.h"
#include "watch_media.h"
#include "watch_script.h"
#include "watch_sensors.h"

static const char *TAG = "WATCH_POWER";

#ifndef CONFIG_WATCH_IDLE_POWEROFF_SEC
#define CONFIG_WATCH_IDLE_POWEROFF_SEC 120
#endif
#ifndef CONFIG_WATCH_LOW_BATT_MV
#define CONFIG_WATCH_LOW_BATT_MV 3400
#endif
#ifndef CONFIG_WATCH_LOW_BATT_RECOVER_MV
#define CONFIG_WATCH_LOW_BATT_RECOVER_MV 3550
#endif
#ifndef CONFIG_WATCH_LOW_BATT_WARN_SEC
#define CONFIG_WATCH_LOW_BATT_WARN_SEC 3
#endif
#ifndef CONFIG_WATCH_IDLE_OFF_WHILE_CHARGING
#define CONFIG_WATCH_IDLE_OFF_WHILE_CHARGING 0
#endif

static int64_t s_last_activity;
static int64_t s_last_poll;
static int s_idle_seconds = CONFIG_WATCH_IDLE_POWEROFF_SEC;
static int s_low_count;
static int s_recover_count;
static bool s_low_active;
static int s_low_countdown;
static int s_low_mv;
static int s_low_pct;
static bool s_powering_off;

bool watch_power_consume_fast_boot(void)
{
    bool fast = watch_config_get_int("boot_fast", 0) != 0;
    if (fast) {
        watch_config_set_int("boot_fast", 0);
    }
    return fast;
}

void watch_power_init(void)
{
    s_idle_seconds = watch_config_get_int("idle_off", CONFIG_WATCH_IDLE_POWEROFF_SEC);
    if (s_idle_seconds < 0) s_idle_seconds = 0;
    s_last_activity = esp_timer_get_time();
    s_last_poll = 0;
    ESP_LOGI(TAG, "idle timeout=%ds low-batt=%dmV warning=%ds",
             s_idle_seconds, CONFIG_WATCH_LOW_BATT_MV, CONFIG_WATCH_LOW_BATT_WARN_SEC);
}

void watch_power_note_activity(void)
{
    s_last_activity = esp_timer_get_time();
}

int watch_power_idle_seconds(void) { return s_idle_seconds; }

void watch_power_set_idle_seconds(int seconds)
{
    if (seconds < 0) seconds = 0;
    s_idle_seconds = seconds;
    watch_config_set_int("idle_off", seconds);
    watch_power_note_activity();
}

bool watch_power_low_batt_active(void) { return s_low_active; }
int watch_power_low_batt_mv(void) { return s_low_mv; }
int watch_power_low_batt_percent(void) { return s_low_pct; }
int watch_power_low_batt_countdown(void) { return s_low_countdown; }

static bool ai_is_active(void)
{
    ws_ai_state_t st = watch_cloud_state();
    return st == WS_AI_LISTENING || st == WS_AI_THINKING || st == WS_AI_SPEAKING ||
           watch_cloud_listening();
}

static bool keep_awake(void)
{
    return watch_media_busy() || watch_script_busy() || ai_is_active();
}

static void low_battery_enter(const ws_power_t *p)
{
    s_low_active = true;
    s_low_countdown = CONFIG_WATCH_LOW_BATT_WARN_SEC;
    s_low_mv = p->batt_mv;
    s_low_pct = p->batt_pct;
    s_recover_count = 0;
    watch_media_stop();
    ESP_LOGW(TAG, "LOW BATTERY: %d mV %d%% (no VBUS), shutdown in %ds",
             s_low_mv, s_low_pct, s_low_countdown);
}

static void low_battery_leave(const char *why)
{
    s_low_active = false;
    s_low_count = 0;
    s_recover_count = 0;
    s_low_countdown = 0;
    watch_power_note_activity();
    ESP_LOGI(TAG, "low battery warning cleared (%s)", why);
}

void watch_power_poll(void)
{
    int64_t now = esp_timer_get_time();
    if (s_powering_off) return;
    if (now - s_last_poll < 1000000) return;
    s_last_poll = now;

    ws_power_t p = watch_sensors_power();
    if (p.batt_mv <= 0 && p.batt_pct <= 0) {
        p.batt_mv = ft_pmic_read_batt_mv();
        p.batt_pct = ft_pmic_battery_percent();
        p.vbus = ft_pmic_vbus_present();
        p.charging = p.vbus;
    }

    if (s_low_active) {
        s_low_mv = p.batt_mv;
        s_low_pct = p.batt_pct;
        if (p.vbus) {
            low_battery_leave("VBUS");
            return;
        }
        if (p.batt_mv > CONFIG_WATCH_LOW_BATT_RECOVER_MV) {
            s_recover_count++;
        } else {
            s_recover_count = 0;
        }
        if (s_recover_count >= 2) {
            low_battery_leave("voltage recovered");
            return;
        }
        if (s_low_countdown > 0) {
            s_low_countdown--;
            ESP_LOGW(TAG, "low battery shutdown in %ds", s_low_countdown);
        }
        if (s_low_countdown <= 0) {
            watch_power_off(WS_POWEROFF_LOW_BATTERY);
        }
        return;
    }

    if (!p.vbus && ft_pmic_battery_present() &&
        p.batt_mv > 0 && p.batt_mv <= CONFIG_WATCH_LOW_BATT_MV) {
        s_low_count++;
    } else {
        s_low_count = 0;
    }
    if (s_low_count >= 5) {
        low_battery_enter(&p);
        return;
    }

    if (s_idle_seconds <= 0) return;
    if (p.vbus && !CONFIG_WATCH_IDLE_OFF_WHILE_CHARGING) return;
    if (keep_awake()) {
        watch_power_note_activity();
        return;
    }
    int64_t idle_us = now - s_last_activity;
    if (idle_us >= (int64_t)s_idle_seconds * 1000000) {
        ESP_LOGI(TAG, "idle for %lld ms, powering off",
                 (long long)(idle_us / 1000));
        watch_power_off(WS_POWEROFF_IDLE);
    }
}

void watch_power_off(ws_poweroff_reason_t reason)
{
    if (s_powering_off) return;
    s_powering_off = true;

    const char *name = reason == WS_POWEROFF_IDLE ? "idle" :
                       reason == WS_POWEROFF_LOW_BATTERY ? "low-battery" : "user";
    ESP_LOGI(TAG, "power off (%s), next boot=%s", name,
             reason == WS_POWEROFF_IDLE ? "fast" : "normal");

    /* Persist the one-shot boot hint before rails go down. */
    watch_config_set_int("boot_fast", reason == WS_POWEROFF_IDLE ? 1 : 0);

    watch_media_stop();
    watch_cloud_stop();
    vTaskDelay(pdMS_TO_TICKS(150));
    ft_lcd_backlight(0);
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_err_t err = ft_pmic_poweroff();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMIC poweroff failed: %s", esp_err_to_name(err));
        esp_restart();
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();   /* only reached if PMIC did not cut power */
}
