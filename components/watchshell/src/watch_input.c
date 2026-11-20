#include "watch_input.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "ft_keys.h"
#include "ft_pmic.h"
#include "ft_touch.h"

static const char *TAG = "WATCH_IN";
static bool s_touch_ready;
static bool s_pmic_ready;
static bool s_pmic_down;
static bool s_pmic_up_pending;
static bool s_pressed;
static int s_x, s_y;
static int s_down_x, s_down_y;
static bool s_key_last[WS_KEY_COUNT];

static ws_key_t map_key(ft_key_t key)
{
    switch (key) {
    case FT_KEY_PWR: return WS_KEY_PWR;
    case FT_KEY_LEFT: return WS_KEY_LEFT;
    case FT_KEY_RIGHT: return WS_KEY_RIGHT;
    default: return WS_KEY_PWR;
    }
}

esp_err_t watch_input_init_keys(void)
{
    esp_err_t err = ft_keys_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "keys init: %s", esp_err_to_name(err));
    }
    s_pmic_ready = ft_pmic_irq_available();
    for (int i = 0; i < WS_KEY_COUNT; i++) {
        s_key_last[i] = false;
    }
    ESP_LOGI(TAG, "keys ready, PWR source %s, GPIO0=%d",
             s_pmic_ready ? "PMIC_INT" : "GPIO0 fallback",
             gpio_get_level(GPIO_BOOT_KEY));
    return err;
}

esp_err_t watch_input_init_touch(void)
{
    esp_err_t err = ft_touch_init();
    s_touch_ready = (err == ESP_OK && ft_touch_present());
    ESP_LOGI(TAG, "touch %s", s_touch_ready ? "ready" : "unavailable");
    return err;
}

esp_err_t watch_input_init(void)
{
    esp_err_t err = watch_input_init_keys();
    esp_err_t touch_err = watch_input_init_touch();
    return err != ESP_OK ? err : touch_err;
}

bool watch_input_key_down(ws_key_t key)
{
    if (key >= WS_KEY_COUNT) return false;
    if (key == WS_KEY_PWR && s_pmic_ready) return s_pmic_down;
    return ft_keys_pressed((ft_key_t)key);
}

bool watch_input_poll(ws_event_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    /* PWR comes from the AXP2101 PWRON edge IRQs.  The IRQ line is latched
     * low until INTSTS is cleared; a read can report both edges at once. */
    if (s_pmic_ready) {
        if (s_pmic_up_pending) {
            s_pmic_up_pending = false;
            s_pmic_down = false;
            out->type = WS_EV_KEY_UP;
            out->key = WS_KEY_PWR;
            return true;
        }
        if (ft_pmic_irq_pending()) {
            uint8_t st[3] = {0};
            if (ft_pmic_read_irq(st) == ESP_OK) {
                ft_pmic_clear_irq();
                bool down = ft_pmic_pwr_down_irq(st);
                bool up = ft_pmic_pwr_up_irq(st);
                if (down) {
                    s_pmic_down = true;
                    s_pmic_up_pending = up;
                    out->type = WS_EV_KEY_DOWN;
                    out->key = WS_KEY_PWR;
                    return true;
                }
                if (up) {
                    s_pmic_down = false;
                    out->type = WS_EV_KEY_UP;
                    out->key = WS_KEY_PWR;
                    return true;
                }
            }
        }
    }

    /* Touch: a single event is returned per call, keys are latched. */
    if (s_touch_ready) {
        ft_touch_data_t t = {0};
        if (ft_touch_read(&t) == ESP_OK) {
            if (t.points > 0) {
                int x = t.x;
                int y = t.y;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
                if (x > WS_SCREEN_W - 1) x = WS_SCREEN_W - 1;
                if (y > WS_SCREEN_H - 1) y = WS_SCREEN_H - 1;
                if (!s_pressed) {
                    s_pressed = true;
                    s_x = x;
                    s_y = y;
                    s_down_x = x;
                    s_down_y = y;
                    out->type = WS_EV_TOUCH_DOWN;
                    out->x = (int16_t)x;
                    out->y = (int16_t)y;
                    return true;
                }
                if (abs(x - s_x) + abs(y - s_y) >= 1) {
                    out->type = WS_EV_TOUCH_MOVE;
                    out->x = (int16_t)x;
                    out->y = (int16_t)y;
                    out->dx = (int16_t)(x - s_x);
                    out->dy = (int16_t)(y - s_y);
                    s_x = x;
                    s_y = y;
                    return true;
                }
            } else if (s_pressed) {
                s_pressed = false;
                out->type = WS_EV_TOUCH_UP;
                out->x = (int16_t)s_x;
                out->y = (int16_t)s_y;
                out->dx = (int16_t)(s_x - s_down_x);
                out->dy = (int16_t)(s_y - s_down_y);
                return true;
            }
        }
    }

    for (int i = 0; i < WS_KEY_COUNT; i++) {
        if (i == FT_KEY_PWR && s_pmic_ready) continue;
        bool now = ft_keys_pressed((ft_key_t)i);
        if (now != s_key_last[i]) {
            s_key_last[i] = now;
            out->type = now ? WS_EV_KEY_DOWN : WS_EV_KEY_UP;
            out->key = (uint8_t)map_key((ft_key_t)i);
            return true;
        }
    }
    return false;
}
