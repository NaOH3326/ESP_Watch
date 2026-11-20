/**
 * @file  ft_touch.h
 * @brief CST816D 触摸控制器（I2C 0x15）
 */
#pragma once

#include "ft_board.h"
#include "ft_i2c.h"

typedef struct {
    uint8_t status;
    uint8_t gesture;
    uint8_t points;
    uint16_t x;
    uint16_t y;
} ft_touch_data_t;

esp_err_t ft_touch_init(void);
bool ft_touch_present(void);

esp_err_t ft_touch_read(ft_touch_data_t *data);
uint8_t ft_touch_chip_id(void);
uint8_t ft_touch_fw_version(void);

void ft_touch_reset(void);
bool ft_touch_int_level(void);
