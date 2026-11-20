#include "watchshell.h"

#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ft_i2c.h"
#include "ft_lcd.h"
#include "ft_pmic.h"
#include "watch_audio.h"
#include "watch_cloud.h"
#include "watch_config.h"
#include "watch_console.h"
#include "watch_gfx.h"
#include "watch_input.h"
#include "watch_media.h"
#include "watch_net.h"
#include "watch_power.h"
#include "watch_script.h"
#include "watch_sensors.h"
#include "watch_storage.h"
#include "watch_term.h"
#include "watch_theme.h"
#include "watch_ui.h"

static const char *TAG = "WATCHSHELL";

static int64_t s_boot_start;
static int64_t s_mark_pmic;
static int64_t s_mark_lcd;

static void splash(bool fast)
{
    watch_gfx_clear(WS_COLOR_BLACK);
    watch_gfx_text(18, 54, "WATCHSHELL", WS_COLOR_GREEN, 3);
    if (fast) {
        watch_gfx_text_center(120, 96, "FAST BOOT", WS_COLOR_CYAN, 1);
        watch_gfx_text_center(120, 116, "UI FIRST / SERVICES IN BACKGROUND", WS_COLOR_DIM, 1);
        watch_gfx_text(20, 184, "root@esp-watch:~#", WS_COLOR_GREEN, 1);
        watch_gfx_flush();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_WATCH_FAST_SPLASH_MS));
        return;
    }
    watch_gfx_text(20, 92, "ESP32-S3 / 16MB / 8MB PSRAM", WS_COLOR_GRAY, 1);
    watch_gfx_hline(20, 108, 200, WS_COLOR_DIM);
    watch_gfx_text(20, 122, "mount /storage ...", WS_COLOR_CYAN, 1);
    watch_gfx_text(20, 134, "init sensors ...", WS_COLOR_CYAN, 1);
    watch_gfx_text(20, 146, "init network ...", WS_COLOR_CYAN, 1);
    watch_gfx_text(20, 158, "start shell ...", WS_COLOR_CYAN, 1);
    watch_gfx_text(20, 184, "root@esp-watch:~#", WS_COLOR_GREEN, 1);
    watch_gfx_flush();
    vTaskDelay(pdMS_TO_TICKS(CONFIG_WATCH_NORMAL_SPLASH_MS));
}

static void boot_services(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    watch_storage_init();
    int64_t t_storage = esp_timer_get_time();
    watch_cloud_init();
    watch_net_init();
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "services: storage=%lldms cloud/net=%lldms total=%lldms",
             (long long)((t_storage - t0) / 1000),
             (long long)((now - t_storage) / 1000),
             (long long)((now - s_boot_start) / 1000));
    ESP_LOGI(TAG, "ready: IP=%s storage=%s", watch_net_ip(),
             watch_storage_ready() ? "mounted" : "failed");
    vTaskDelete(NULL);
}

esp_err_t watchshell_start(void)
{
    s_boot_start = esp_timer_get_time();
    ESP_LOGI(TAG, "booting WatchShell");
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_ERROR_CHECK(watch_config_init());

    bool fast = watch_power_consume_fast_boot();
    ESP_LOGI(TAG, "boot mode: %s", fast ? "FAST (idle power-off)" : "NORMAL");

    esp_err_t err = ft_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c: %s", esp_err_to_name(err));
    }
    err = ft_pmic_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMIC: %s", esp_err_to_name(err));
    }
    s_mark_pmic = esp_timer_get_time();
    err = watch_gfx_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display: %s", esp_err_to_name(err));
        return err;
    }
    s_mark_lcd = esp_timer_get_time();
    watch_theme_init();
    watch_power_init();
    splash(fast);

    watch_term_init();
    watch_media_init();
    watch_script_init();
    if (fast) {
        /* First frame ASAP: keys/PMIC are cheap, touch/IMU come afterwards. */
        watch_input_init_keys();
        watch_sensors_time_refresh();
        watch_ui_start();
        watch_input_init_touch();
        watch_sensors_init();
        watch_console_start();
    } else {
        watch_input_init();
        watch_sensors_init();
        watch_ui_start();
        watch_console_start();
    }
    ESP_LOGI(TAG, "boot: fast=%d pmic=%lldms lcd=%lldms ui=%lldms",
             fast, (long long)((s_mark_pmic - s_boot_start) / 1000),
             (long long)((s_mark_lcd - s_mark_pmic) / 1000),
             (long long)((esp_timer_get_time() - s_mark_lcd) / 1000));

    if (fast) {
        BaseType_t ok = xTaskCreatePinnedToCore(boot_services, "boot_svc", 8192,
                                                NULL, 3, NULL, 0);
        if (ok != pdPASS) {
            ESP_LOGW(TAG, "background boot task failed, running synchronously");
            boot_services(NULL);
        }
    } else {
        boot_services(NULL);
    }
    return ESP_OK;
}
