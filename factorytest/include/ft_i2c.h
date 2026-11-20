/**
 * @file  ft_i2c.h
 * @brief 两路 I2C 总线与通用寄存器读写
 */
#pragma once

#include "ft_board.h"
#include "driver/i2c_master.h"

typedef struct {
    i2c_master_dev_handle_t dev;
    uint16_t addr;
    bool touch_bus;
} ft_i2c_dev_t;

esp_err_t ft_i2c_init(void);
void ft_i2c_deinit(void);

i2c_master_bus_handle_t ft_i2c_bus(bool touch_bus);

esp_err_t ft_i2c_add(ft_i2c_dev_t *out, uint16_t addr, bool touch_bus);
esp_err_t ft_i2c_del(ft_i2c_dev_t *dev);

esp_err_t ft_i2c_read(ft_i2c_dev_t *dev, uint8_t *buf, size_t len);
esp_err_t ft_i2c_write(ft_i2c_dev_t *dev, const uint8_t *buf, size_t len);

esp_err_t ft_i2c_read_reg(ft_i2c_dev_t *dev, uint8_t reg, uint8_t *buf, size_t len);
esp_err_t ft_i2c_write_reg(ft_i2c_dev_t *dev, uint8_t reg, const uint8_t *buf, size_t len);
esp_err_t ft_i2c_read_reg8(ft_i2c_dev_t *dev, uint8_t reg, uint8_t *val);
esp_err_t ft_i2c_write_reg8(ft_i2c_dev_t *dev, uint8_t reg, uint8_t val);

bool ft_i2c_probe(uint16_t addr, bool touch_bus);
int ft_i2c_scan(bool touch_bus);
const char *ft_i2c_addr_name(uint16_t addr);
