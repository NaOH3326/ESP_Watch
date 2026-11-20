/**
 * @file  ft_keys.c
 * @brief 按键轮询（内部上拉，按下为低）
 */
#include "ft_keys.h"

#include "esp_timer.h"

static const gpio_num_t s_pins[FT_KEY_MAX] = {
    GPIO_BOOT_KEY,
    GPIO_KEY_LEFT,
    GPIO_KEY_RIGHT,
};

static const char *s_names[FT_KEY_MAX] = {
    "PWR/BOOT",
    "LEFT",
    "RIGHT",
};

const char *ft_key_name(ft_key_t key)
{
    return (key < FT_KEY_MAX) ? s_names[key] : "?";
}

esp_err_t ft_keys_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_BOOT_KEY) | (1ULL << GPIO_KEY_LEFT) | (1ULL << GPIO_KEY_RIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool ft_keys_pressed(ft_key_t key)
{
    if (key >= FT_KEY_MAX) {
        return false;
    }
    return gpio_get_level(s_pins[key]) == 0;
}

void ft_keys_print(void)
{
    printf("  keys: ");
    for (int i = 0; i < FT_KEY_MAX; i++) {
        printf("%s=%s%s", s_names[i], ft_keys_pressed((ft_key_t)i) ? "DOWN" : "up",
               (i + 1 < FT_KEY_MAX) ? "  " : "\n");
    }
}

int ft_keys_stream(int seconds)
{
    if (seconds <= 0) {
        seconds = 10;
    }
    bool last[FT_KEY_MAX];
    for (int i = 0; i < FT_KEY_MAX; i++) {
        last[i] = ft_keys_pressed((ft_key_t)i);
    }
    int events = 0;
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    while (esp_timer_get_time() < end) {
        for (int i = 0; i < FT_KEY_MAX; i++) {
            bool now = ft_keys_pressed((ft_key_t)i);
            if (now != last[i]) {
                printf("  [%8lld ms] %-9s %s\n", esp_timer_get_time() / 1000,
                       s_names[i], now ? "DOWN" : "up");
                last[i] = now;
                events++;
            }
        }
        ft_delay_ms(20);
    }
    printf("  key events: %d\n", events);
    return events;
}
