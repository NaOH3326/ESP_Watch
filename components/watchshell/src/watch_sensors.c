#include "watch_sensors.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ft_i2c.h"
#include "ft_pmic.h"
#include "ft_sensors.h"
#include "watch_config.h"

static const char *TAG = "WATCH_SENS";

#define QMC_ADDR 0x2C
#define QMC_CHIP_ID 0x00
#define QMC_DATA_X_LSB 0x01
#define QMC_STATUS 0x09
#define QMC_CMD1 0x0A
#define QMC_CMD2 0x0B

static ft_i2c_dev_t s_mag_dev;
static bool s_mag_ready;
static ws_imu_t s_imu;
static ws_mag_t s_mag;
static ws_env_t s_env;
static ws_time_t s_time;
static int64_t s_last_imu;
static int64_t s_last_mag;
static int64_t s_last_env;
static int64_t s_last_time;
static int64_t s_step_hold;
static float s_mag_off[3];
static float s_mag_scale[3] = {1, 1, 1};
static bool s_above;
static TaskHandle_t s_poll_task;
static volatile bool s_calibrating;
static void sensor_poll_task(void *arg);

static esp_err_t mag_write(uint8_t reg, uint8_t val)
{
    return ft_i2c_write_reg8(&s_mag_dev, reg, val);
}

static esp_err_t mag_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return ft_i2c_read_reg(&s_mag_dev, reg, buf, len);
}

static void mag_init(void)
{
    if (ft_i2c_add(&s_mag_dev, QMC_ADDR, false) != ESP_OK) {
        ESP_LOGW(TAG, "QMC5883P add failed");
        return;
    }
    uint8_t id = 0;
    if (mag_read(QMC_CHIP_ID, &id, 1) != ESP_OK || id != 0x80) {
        ESP_LOGW(TAG, "QMC5883P not found id=0x%02X", id);
        return;
    }
    mag_write(QMC_CMD2, 0x80);
    vTaskDelay(pdMS_TO_TICKS(20));
    mag_write(QMC_CMD2, 0x00);
    mag_write(QMC_CMD2, 0x08);   /* 8 G range, OSR 8, DSR 1 */
    mag_write(QMC_CMD1, 0x07);   /* 50 Hz continuous */
    vTaskDelay(pdMS_TO_TICKS(30));
    s_mag_ready = true;
    ESP_LOGI(TAG, "QMC5883P ready (id=0x%02X)", id);
}

void watch_sensors_init(void)
{
    esp_err_t err = ft_sensors_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init: %s", esp_err_to_name(err));
    }
    memset(&s_imu, 0, sizeof(s_imu));
    memset(&s_mag, 0, sizeof(s_mag));
    memset(&s_env, 0, sizeof(s_env));
    memset(&s_time, 0, sizeof(s_time));
    s_last_time = 0;
    mag_init();
    if (!ft_rtc_is_valid()) {
        ESP_LOGW(TAG, "RTC invalid, seeding from build time");
        esp_err_t rtc_err = ft_rtc_set_compile_time();
        if (rtc_err != ESP_OK) {
            ESP_LOGW(TAG, "RTC seed failed: %s", esp_err_to_name(rtc_err));
        }
    }
    watch_sensors_time_refresh();

    s_mag_off[0] = (float)watch_config_get_int("mag_ox", 0);
    s_mag_off[1] = (float)watch_config_get_int("mag_oy", 0);
    s_mag_off[2] = (float)watch_config_get_int("mag_oz", 0);
    if (!s_poll_task) {
        xTaskCreatePinnedToCore(sensor_poll_task, "sensors", 4096, NULL, 4, &s_poll_task, 0);
    }
}

static void sensor_poll_task(void *arg)
{
    (void)arg;
    while (1) {
        watch_sensors_poll();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void poll_imu(int64_t now)
{
    if (now - s_last_imu < 20 * 1000) return;
    s_last_imu = now;
    ft_imu_sample_t sample;
    if (ft_imu_read(&sample) != ESP_OK) {
        s_imu.ok = false;
        return;
    }
    s_imu.ok = true;
    s_imu.ax = sample.accel_g[0];
    s_imu.ay = sample.accel_g[1];
    s_imu.az = sample.accel_g[2];
    s_imu.gx = sample.gyro_dps[0];
    s_imu.gy = sample.gyro_dps[1];
    s_imu.gz = sample.gyro_dps[2];
    s_imu.temp_c = sample.temp_c;

    float mag = sqrtf(s_imu.ax * s_imu.ax + s_imu.ay * s_imu.ay + s_imu.az * s_imu.az);
    if (mag > 1.22f && !s_above && now - s_step_hold > 280 * 1000) {
        s_imu.steps++;
        s_step_hold = now;
        s_above = true;
    } else if (mag < 1.05f) {
        s_above = false;
    }
}

static void poll_mag(int64_t now)
{
    if (s_calibrating || !s_mag_ready || now - s_last_mag < 50 * 1000) return;
    s_last_mag = now;
    uint8_t st = 0;
    if (mag_read(QMC_STATUS, &st, 1) != ESP_OK || !(st & 0x01)) {
        s_mag.age_ms += 50;
        return;
    }
    uint8_t b[6];
    if (mag_read(QMC_DATA_X_LSB, b, sizeof(b)) != ESP_OK) {
        s_mag.ok = false;
        return;
    }
    int16_t x = (int16_t)((b[1] << 8) | b[0]);
    int16_t y = (int16_t)((b[3] << 8) | b[2]);
    int16_t z = (int16_t)((b[5] << 8) | b[4]);
    s_mag.x = x;
    s_mag.y = y;
    s_mag.z = z;
    s_mag.age_ms = 0;
    s_mag.ok = true;

    float mx = (x - s_mag_off[0]) * s_mag_scale[0];
    float my = (y - s_mag_off[1]) * s_mag_scale[1];
    float mz = (z - s_mag_off[2]) * s_mag_scale[2];
    float ax = s_imu.ax, ay = s_imu.ay, az = s_imu.az;
    float roll = atan2f(ay, az);
    float pitch = atan2f(-ax, sqrtf(ay * ay + az * az));
    float xh = mx * cosf(pitch) + mz * sinf(pitch);
    float yh = mx * sinf(roll) * sinf(pitch) + my * cosf(roll) - mz * sinf(roll) * cosf(pitch);
    float heading = atan2f(-yh, xh) * 180.0f / 3.14159265f;
    if (heading < 0) heading += 360.0f;
    if (heading >= 360.0f) heading -= 360.0f;
    s_mag.heading_deg = heading;
}

static void poll_env(int64_t now)
{
    if (now - s_last_env < 2000 * 1000) return;
    s_last_env = now;
    float t, rh;
    if (ft_aht_read(&t, &rh) == ESP_OK) {
        s_env.ok = true;
        s_env.temp_c = t;
        s_env.rh = rh;
    } else {
        s_env.ok = false;
    }
}

static void poll_time(int64_t now)
{
    if (now - s_last_time < 1000 * 1000) return;
    s_last_time = now;
    ft_rtc_raw_t raw;
    if (ft_rtc_read(&raw) == ESP_OK) {
        int y, mo, d, h, mi, sec;
        ft_rtc_to_decimal(&raw, &y, &mo, &d, &h, &mi, &sec);
        if (y >= 2020 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
            s_time.year = y; s_time.month = mo; s_time.day = d;
            s_time.hour = h; s_time.minute = mi; s_time.second = sec;
            s_time.valid = true;
            return;
        }
    }
    s_time.valid = false;
}

void watch_sensors_poll(void)
{
    int64_t now = esp_timer_get_time();
    poll_imu(now);
    poll_mag(now);
    poll_env(now);
    poll_time(now);
}

void watch_sensors_time_refresh(void)
{
    if (!ft_rtc_is_valid()) {
        ft_rtc_set_compile_time();
    }
    s_last_time = 0;
    poll_time(esp_timer_get_time());
}

const ws_imu_t *watch_sensors_imu(void) { return &s_imu; }
const ws_mag_t *watch_sensors_mag(void) { return &s_mag; }
const ws_env_t *watch_sensors_env(void) { return &s_env; }
const ws_time_t *watch_sensors_time(void) { return &s_time; }

ws_power_t watch_sensors_power(void)
{
    static ws_power_t cached;
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (now - last < 1000 * 1000) {
        return cached;
    }
    last = now;
    cached.batt_mv = ft_pmic_read_batt_mv();
    cached.batt_pct = ft_pmic_battery_percent();
    cached.vsys_mv = ft_pmic_read_vsys_mv();
    cached.vbus_mv = ft_pmic_read_vbus_mv();
    cached.charging = ft_pmic_vbus_present();
    cached.vbus = cached.charging;
    if (cached.batt_pct < 0) cached.batt_pct = 0;
    return cached;
}

bool watch_sensors_calibrate_mag(int seconds)
{
    if (!s_mag_ready) return false;
    s_calibrating = true;
    int16_t minx = 32767, maxx = -32768;
    int16_t miny = 32767, maxy = -32768;
    int16_t minz = 32767, maxz = -32768;
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    while (esp_timer_get_time() < end) {
        uint8_t st = 0;
        if (mag_read(QMC_STATUS, &st, 1) == ESP_OK && (st & 1)) {
            uint8_t b[6];
            if (mag_read(QMC_DATA_X_LSB, b, sizeof(b)) == ESP_OK) {
                int16_t x = (int16_t)((b[1] << 8) | b[0]);
                int16_t y = (int16_t)((b[3] << 8) | b[2]);
                int16_t z = (int16_t)((b[5] << 8) | b[4]);
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
                if (z < minz) minz = z;
                if (z > maxz) maxz = z;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_calibrating = false;
    if (maxx <= minx || maxy <= miny || maxz <= minz) return false;
    s_mag_off[0] = (minx + maxx) * 0.5f;
    s_mag_off[1] = (miny + maxy) * 0.5f;
    s_mag_off[2] = (minz + maxz) * 0.5f;
    float rx = (maxx - minx) * 0.5f;
    float ry = (maxy - miny) * 0.5f;
    float rz = (maxz - minz) * 0.5f;
    s_mag_scale[0] = 1.0f;
    s_mag_scale[1] = rx / (ry > 1 ? ry : 1);
    s_mag_scale[2] = rx / (rz > 1 ? rz : 1);
    watch_config_set_int("mag_ox", (int)s_mag_off[0]);
    watch_config_set_int("mag_oy", (int)s_mag_off[1]);
    watch_config_set_int("mag_oz", (int)s_mag_off[2]);
    return true;
}
