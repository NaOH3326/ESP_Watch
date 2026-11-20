/**
 * @file  ft_sensors.h
 * @brief 传感器与 RTC：ICM-42670 / QMC5883P / AHT20 / PCF85063
 */
#pragma once

#include <time.h>

#include "ft_board.h"
#include "ft_i2c.h"

typedef struct {
    int16_t accel_raw[3];
    int16_t gyro_raw[3];
    int16_t temp_raw;
    float accel_g[3];
    float gyro_dps[3];
    float temp_c;
} ft_imu_sample_t;

typedef struct {
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t days;
    uint8_t weekdays;
    uint8_t months;
    uint8_t years;
} ft_rtc_raw_t;

esp_err_t ft_sensors_init(void);

/* IMU (SPI) */
esp_err_t ft_imu_init(void);
esp_err_t ft_imu_probe(uint8_t *who_am_i);
esp_err_t ft_imu_read(ft_imu_sample_t *sample);

/* 磁力计 (I2C) */
esp_err_t ft_mag_probe(uint8_t *chip_id, uint8_t *status);

/* 温湿度 (I2C) */
esp_err_t ft_aht_probe(uint8_t *status);
esp_err_t ft_aht_read(float *temp_c, float *rh);

/* RTC (I2C) */
esp_err_t ft_rtc_probe(uint8_t *ctrl1);
esp_err_t ft_rtc_read(ft_rtc_raw_t *raw);
esp_err_t ft_rtc_set_datetime(int year, int month, int day,
                              int hour, int minute, int second);
esp_err_t ft_rtc_set_epoch(time_t epoch);
esp_err_t ft_rtc_set_compile_time(void);
bool ft_rtc_is_valid(void);
esp_err_t ft_rtc_set_default(void);
esp_err_t ft_rtc_check_tick(int *delta_seconds);
esp_err_t ft_rtc_timer_int_test(void);
void ft_rtc_to_decimal(const ft_rtc_raw_t *raw, int *year, int *month, int *day,
                       int *hour, int *minute, int *second);
