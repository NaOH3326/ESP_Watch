/**
 * @file  ft_keys.h
 * @brief 板载按键：电源/BOOT(GPIO0)、左(GPIO45)、右(GPIO40)
 */
#pragma once

#include "ft_board.h"

typedef enum {
    FT_KEY_PWR = 0,
    FT_KEY_LEFT,
    FT_KEY_RIGHT,
    FT_KEY_MAX,
} ft_key_t;

esp_err_t ft_keys_init(void);
bool ft_keys_pressed(ft_key_t key);
void ft_keys_print(void);
int ft_keys_stream(int seconds);
const char *ft_key_name(ft_key_t key);
