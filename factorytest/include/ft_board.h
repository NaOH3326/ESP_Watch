/**
 * @file  ft_board.h
 * @brief ESP_Watch 工厂测试公共定义
 *
 * 本组件随主工程一起构建，复用 main/board_pins.h 的引脚映射，
 * 由 main.c 在 CONFIG_FACTORY_TEST_MODE=y 时调用 factory_test_start()。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"

/* 唯一真源：生产工程的引脚表 */
#include "../../main/board_pins.h"

#define FT_TAG "FACTORY"

/* ---------------- I2C ---------------- */
#define FT_I2C_SENSOR_PORT   I2C_NUM_0
#define FT_I2C_TOUCH_PORT    I2C_NUM_1
#define FT_I2C_FREQ_HZ       400000
#define FT_I2C_TIMEOUT_MS    100

#define FT_I2C_ADDR_AXP2101  0x34
#define FT_I2C_ADDR_QMC5883P 0x2C
#define FT_I2C_ADDR_AHT20    0x38
#define FT_I2C_ADDR_PCF85063 0x51
#define FT_I2C_ADDR_CST816D  0x15

/* ---------------- 面板默认参数（Kconfig 可覆盖） ---------------- */
#ifndef CONFIG_FT_LCD_WIDTH
#define CONFIG_FT_LCD_WIDTH 240
#endif
#ifndef CONFIG_FT_LCD_HEIGHT
#define CONFIG_FT_LCD_HEIGHT 284
#endif
#ifndef CONFIG_FT_LCD_X_OFFSET
#define CONFIG_FT_LCD_X_OFFSET 0
#endif
#ifndef CONFIG_FT_LCD_Y_OFFSET
#define CONFIG_FT_LCD_Y_OFFSET 0
#endif
#ifndef CONFIG_FT_LCD_SPI_MHZ
#define CONFIG_FT_LCD_SPI_MHZ 40
#endif
#ifndef CONFIG_FT_ECHO_GAIN_X10
#define CONFIG_FT_ECHO_GAIN_X10 8
#endif
#ifndef CONFIG_FT_LCD_MAX_BRIGHTNESS
#define CONFIG_FT_LCD_MAX_BRIGHTNESS 100
#endif

/* ---------------- 各模块初始化返回值汇总 ---------------- */
typedef struct {
    bool i2c;
    bool pmic;
    bool lcd;
    bool touch;
    bool imu;
    bool mag;
    bool aht;
    bool rtc;
    bool keys;
} ft_presence_t;

extern ft_presence_t g_ft_present;

/* 工具函数 */
static inline uint16_t ft_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void ft_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}
