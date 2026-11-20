#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ok;
    float ax, ay, az;
    float gx, gy, gz;
    float temp_c;
    int steps;
} ws_imu_t;

typedef struct {
    bool ok;
    int16_t x, y, z;
    float heading_deg;
    uint32_t age_ms;
} ws_mag_t;

typedef struct {
    bool ok;
    float temp_c;
    float rh;
} ws_env_t;

typedef struct {
    int year, month, day, hour, minute, second;
    bool valid;
} ws_time_t;

typedef struct {
    int batt_mv;
    int batt_pct;
    int vsys_mv;
    int vbus_mv;
    bool charging;
    bool vbus;
} ws_power_t;

void watch_sensors_init(void);
void watch_sensors_poll(void);
/* Fast boot: read PCF85063 once without initializing IMU/mag/env tasks. */
void watch_sensors_time_refresh(void);

const ws_imu_t *watch_sensors_imu(void);
const ws_mag_t *watch_sensors_mag(void);
const ws_env_t *watch_sensors_env(void);
const ws_time_t *watch_sensors_time(void);
ws_power_t watch_sensors_power(void);

/* Blocking calibration: rotate the watch for `seconds`; offsets are saved in NVS. */
bool watch_sensors_calibrate_mag(int seconds);
