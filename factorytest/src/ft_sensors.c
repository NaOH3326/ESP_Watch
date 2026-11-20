/**
 * @file  ft_sensors.c
 * @brief ICM-42670-P(SPI) / QMC5883P / AHT20 / PCF85063
 */
#include "ft_sensors.h"

#include "driver/spi_master.h"

static const char *TAG = "FT_SENSOR";

/* ==================================================================== */
/* ICM-42670-P                                                          */
/* ==================================================================== */
#define ICM_REG_TEMP_DATA1   0x09
#define ICM_REG_ACCEL_X1     0x0B
#define ICM_REG_GYRO_X1      0x11
#define ICM_REG_PWR_MGMT0    0x1F
#define ICM_REG_GYRO_CONFIG0 0x20
#define ICM_REG_ACCEL_CONFIG0 0x21
#define ICM_REG_WHO_AM_I     0x75
#define ICM_WHO_AM_I_VAL     0x67

static spi_device_handle_t s_imu;
static bool s_imu_bus_ready;

static esp_err_t imu_bus_init(void)
{
    if (s_imu_bus_ready) {
        return ESP_OK;
    }
    spi_bus_config_t buscfg = {
        .mosi_io_num = GPIO_IMU_MOSI,
        .miso_io_num = GPIO_IMU_MISO,
        .sclk_io_num = GPIO_IMU_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "IMU spi bus init failed: %s", esp_err_to_name(err));
        return err;
    }
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = GPIO_IMU_CS,
        .queue_size = 1,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
    };
    err = spi_bus_add_device(SPI3_HOST, &devcfg, &s_imu);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IMU add device failed: %s", esp_err_to_name(err));
        return err;
    }
    s_imu_bus_ready = true;
    return ESP_OK;
}

static esp_err_t imu_read(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!s_imu || !buf || len + 1 > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[16] = {0};
    uint8_t rx[16] = {0};
    tx[0] = (uint8_t)(reg | 0x80);
    memset(&tx[1], 0xFF, len);
    spi_transaction_t t = {
        .length = 8 * (len + 1),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_imu, &t);
    if (err == ESP_OK) {
        memcpy(buf, &rx[1], len);
    }
    return err;
}

static esp_err_t imu_write(uint8_t reg, uint8_t val)
{
    if (!s_imu) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t tx[2] = { (uint8_t)(reg & 0x7F), val };
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(s_imu, &t);
}

esp_err_t ft_imu_probe(uint8_t *who_am_i)
{
    if (!who_am_i) {
        return ESP_ERR_INVALID_ARG;
    }
    return imu_read(ICM_REG_WHO_AM_I, who_am_i, 1);
}

esp_err_t ft_imu_read(ft_imu_sample_t *sample)
{
    if (!sample) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[12] = {0};
    uint8_t t[2] = {0};
    esp_err_t err = imu_read(ICM_REG_ACCEL_X1, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    err = imu_read(ICM_REG_TEMP_DATA1, t, sizeof(t));
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 3; i++) {
        sample->accel_raw[i] = (int16_t)((b[i * 2] << 8) | b[i * 2 + 1]);
        sample->gyro_raw[i] = (int16_t)((b[6 + i * 2] << 8) | b[6 + i * 2 + 1]);
        /* 默认 ±16g / ±2000dps */
        sample->accel_g[i] = sample->accel_raw[i] / 2048.0f;
        sample->gyro_dps[i] = sample->gyro_raw[i] / 16.4f;
    }
    sample->temp_raw = (int16_t)((t[0] << 8) | t[1]);
    sample->temp_c = sample->temp_raw / 128.0f + 25.0f;
    return ESP_OK;
}

esp_err_t ft_imu_init(void)
{
    esp_err_t err = imu_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t who = 0;
    err = ft_imu_probe(&who);
    if (err != ESP_OK || who != ICM_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "ICM-42670 not found (who=0x%02X err=%s)", who, esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    imu_write(ICM_REG_GYRO_CONFIG0, 0x06);   /* 1 kHz, ±2000 dps */
    imu_write(ICM_REG_ACCEL_CONFIG0, 0x06);  /* 1 kHz, ±16 g */
    imu_write(ICM_REG_PWR_MGMT0, 0x0F);      /* accel + gyro low-noise */
    ft_delay_ms(50);
    ESP_LOGI(TAG, "ICM-42670-P ready (WHO_AM_I=0x%02X)", who);
    return ESP_OK;
}

/* ==================================================================== */
/* QMC5883P                                                             */
/* ==================================================================== */
#define QMC_REG_CHIP_ID 0x00
#define QMC_REG_STATUS  0x09

static ft_i2c_dev_t s_mag;

esp_err_t ft_mag_probe(uint8_t *chip_id, uint8_t *status)
{
    if (!chip_id) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mag.dev) {
        esp_err_t err = ft_i2c_add(&s_mag, FT_I2C_ADDR_QMC5883P, false);
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_err_t err = ft_i2c_read_reg8(&s_mag, QMC_REG_CHIP_ID, chip_id);
    if (err != ESP_OK) {
        return err;
    }
    if (status) {
        ft_i2c_read_reg8(&s_mag, QMC_REG_STATUS, status);
    }
    return ESP_OK;
}

/* ==================================================================== */
/* AHT20                                                                */
/* ==================================================================== */
#define AHT_REG_STATUS 0x71
#define AHT_CMD_INIT   0xBE
#define AHT_CMD_TRIG   0xAC

static ft_i2c_dev_t s_aht;

esp_err_t ft_aht_probe(uint8_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_aht.dev) {
        esp_err_t err = ft_i2c_add(&s_aht, FT_I2C_ADDR_AHT20, false);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ft_i2c_read_reg8(&s_aht, AHT_REG_STATUS, status);
}

esp_err_t ft_aht_read(float *temp_c, float *rh)
{
    if (!temp_c || !rh) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_aht.dev) {
        esp_err_t err = ft_i2c_add(&s_aht, FT_I2C_ADDR_AHT20, false);
        if (err != ESP_OK) {
            return err;
        }
    }
    uint8_t status = 0;
    esp_err_t err = ft_i2c_read_reg8(&s_aht, AHT_REG_STATUS, &status);
    if (err != ESP_OK) {
        return err;
    }
    if ((status & 0x18) != 0x18) {
        /* 初始化命令是纯字节流：0xBE 0x08 0x00，不能走“寄存器+参数”封装 */
        uint8_t init_cmd[3] = { AHT_CMD_INIT, 0x08, 0x00 };
        err = ft_i2c_write(&s_aht, init_cmd, sizeof(init_cmd));
        if (err != ESP_OK) {
            return err;
        }
        ft_delay_ms(10);
    }

    /* 触发测量：0xAC 0x33 0x00，等待 80ms 以上；部分模块只返回 6 字节 */
    uint8_t trig[3] = { AHT_CMD_TRIG, 0x33, 0x00 };
    uint8_t b[6] = {0};
    for (int attempt = 0; attempt < 3; attempt++) {
        err = ft_i2c_write(&s_aht, trig, sizeof(trig));
        if (err == ESP_OK) {
            ft_delay_ms(100);
            err = ft_i2c_read(&s_aht, b, sizeof(b));
        }
        if (err == ESP_OK && (b[0] & 0x80) == 0) {
            uint32_t hraw = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4);
            uint32_t traw = (((uint32_t)(b[3] & 0x0F)) << 16) | ((uint32_t)b[4] << 8) | b[5];
            *rh = hraw * 100.0f / 1048576.0f;
            *temp_c = traw * 200.0f / 1048576.0f - 50.0f;
            return ESP_OK;
        }
        ft_delay_ms(30);
    }
    return err != ESP_OK ? err : ESP_ERR_TIMEOUT;
}


/* ==================================================================== */
/* PCF85063                                                             */
/* ==================================================================== */
#define RTC_REG_CTRL1   0x00
#define RTC_REG_CTRL2   0x01
#define RTC_REG_SECONDS 0x04
#define RTC_REG_TIMER_VALUE 0x10
#define RTC_REG_TIMER_MODE  0x11

#define RTC_CTRL1_STOP  0x20
#define RTC_CTRL2_AIE   0x80
#define RTC_CTRL2_TF    0x08

#define RTC_TIMER_TI_TP   0x01
#define RTC_TIMER_TIE     0x02
#define RTC_TIMER_TE      0x04
#define RTC_TIMER_TCF_1HZ (2 << 3)

static ft_i2c_dev_t s_rtc;

static uint8_t bcd2dec(uint8_t v)
{
    return (uint8_t)(((v >> 4) * 10) + (v & 0x0F));
}

static uint8_t dec2bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

esp_err_t ft_rtc_probe(uint8_t *ctrl1)
{
    if (!ctrl1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_rtc.dev) {
        esp_err_t err = ft_i2c_add(&s_rtc, FT_I2C_ADDR_PCF85063, false);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ft_i2c_read_reg8(&s_rtc, RTC_REG_CTRL1, ctrl1);
}

esp_err_t ft_rtc_read(ft_rtc_raw_t *raw)
{
    if (!raw) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_rtc.dev) {
        esp_err_t err = ft_i2c_add(&s_rtc, FT_I2C_ADDR_PCF85063, false);
        if (err != ESP_OK) {
            return err;
        }
    }
    uint8_t b[7] = {0};
    esp_err_t err = ft_i2c_read_reg(&s_rtc, RTC_REG_SECONDS, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    raw->seconds = b[0];
    raw->minutes = b[1];
    raw->hours = b[2];
    raw->days = b[3];
    raw->weekdays = b[4];
    raw->months = b[5];
    raw->years = b[6];
    return ESP_OK;
}

void ft_rtc_to_decimal(const ft_rtc_raw_t *raw, int *year, int *month, int *day,
                       int *hour, int *minute, int *second)
{
    if (!raw) {
        return;
    }
    if (year)   *year = 2000 + bcd2dec(raw->years);
    if (month)  *month = bcd2dec(raw->months & 0x1F);
    if (day)    *day = bcd2dec(raw->days & 0x3F);
    if (hour)   *hour = bcd2dec(raw->hours & 0x3F);
    if (minute) *minute = bcd2dec(raw->minutes & 0x7F);
    if (second) *second = bcd2dec(raw->seconds & 0x7F);
}

static uint8_t rtc_weekday(int year, int month, int day)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) year -= 1;
    int w = (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
    return (uint8_t)(w < 0 ? w + 7 : w);
}

esp_err_t ft_rtc_set_datetime(int year, int month, int day,
                              int hour, int minute, int second)
{
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_rtc.dev) {
        esp_err_t err = ft_i2c_add(&s_rtc, FT_I2C_ADDR_PCF85063, false);
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_err_t err = ft_i2c_write_reg8(&s_rtc, RTC_REG_CTRL1, RTC_CTRL1_STOP);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t t[7] = {
        dec2bcd((uint8_t)second),
        dec2bcd((uint8_t)minute),
        dec2bcd((uint8_t)hour),
        dec2bcd((uint8_t)day),
        rtc_weekday(year, month, day),
        dec2bcd((uint8_t)month),
        dec2bcd((uint8_t)(year - 2000)),
    };
    err = ft_i2c_write_reg(&s_rtc, RTC_REG_SECONDS, t, sizeof(t));
    esp_err_t start_err = ft_i2c_write_reg8(&s_rtc, RTC_REG_CTRL1, 0x00);
    return err == ESP_OK ? start_err : err;
}

esp_err_t ft_rtc_set_epoch(time_t epoch)
{
    struct tm tm = {0};
    if (!localtime_r(&epoch, &tm)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ft_rtc_set_datetime(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                               tm.tm_hour, tm.tm_min, tm.tm_sec);
}

esp_err_t ft_rtc_set_compile_time(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 1, year = 2026;
    int hour = 12, minute = 0, second = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = strstr(months, mon);
    int month = p ? (int)((p - months) / 3) + 1 : 1;
    ESP_LOGI(TAG, "seeding RTC from build time %04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, minute, second);
    return ft_rtc_set_datetime(year, month, day, hour, minute, second);
}

bool ft_rtc_is_valid(void)
{
    if (!s_rtc.dev) {
        esp_err_t err = ft_i2c_add(&s_rtc, FT_I2C_ADDR_PCF85063, false);
        if (err != ESP_OK) {
            return false;
        }
    }
    uint8_t ctrl1 = 0;
    if (ft_i2c_read_reg8(&s_rtc, RTC_REG_CTRL1, &ctrl1) != ESP_OK ||
        (ctrl1 & RTC_CTRL1_STOP)) {
        return false;
    }
    ft_rtc_raw_t raw = {0};
    if (ft_rtc_read(&raw) != ESP_OK) {
        return false;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    ft_rtc_to_decimal(&raw, &year, &month, &day, &hour, &minute, &second);
    return year >= 2020 && year <= 2099 && month >= 1 && month <= 12 &&
           day >= 1 && day <= 31 && hour < 24 && minute < 60 && second < 60;
}

esp_err_t ft_rtc_set_default(void)
{
    if (!s_rtc.dev) {
        esp_err_t err = ft_i2c_add(&s_rtc, FT_I2C_ADDR_PCF85063, false);
        if (err != ESP_OK) {
            return err;
        }
    }
    /* 停止计时 -> 写入时间 -> 启动 */
    esp_err_t err = ft_i2c_write_reg8(&s_rtc, RTC_REG_CTRL1, RTC_CTRL1_STOP);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t t[7] = {
        dec2bcd(0),    /* 秒 */
        dec2bcd(0),    /* 分 */
        dec2bcd(12),   /* 时 */
        dec2bcd(18),   /* 日 */
        3,             /* 星期（自定义，2026-11-18 周三） */
        dec2bcd(11),   /* 月 */
        dec2bcd(26),   /* 年（2000+26） */
    };
    err = ft_i2c_write_reg(&s_rtc, RTC_REG_SECONDS, t, sizeof(t));
    if (err != ESP_OK) {
        return err;
    }
    return ft_i2c_write_reg8(&s_rtc, RTC_REG_CTRL1, 0x00);
}

esp_err_t ft_rtc_check_tick(int *delta_seconds)
{
    ft_rtc_raw_t a = {0}, b = {0};
    esp_err_t err = ft_rtc_read(&a);
    if (err != ESP_OK) {
        return err;
    }
    ft_delay_ms(1200);
    err = ft_rtc_read(&b);
    if (err != ESP_OK) {
        return err;
    }
    int s1 = bcd2dec(a.seconds & 0x7F);
    int s2 = bcd2dec(b.seconds & 0x7F);
    int delta = (s2 - s1 + 60) % 60;
    if (delta_seconds) {
        *delta_seconds = delta;
    }
    return (delta >= 1 && delta <= 5) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t ft_rtc_timer_int_test(void)
{
    if (!s_rtc.dev) {
        esp_err_t err = ft_i2c_add(&s_rtc, FT_I2C_ADDR_PCF85063, false);
        if (err != ESP_OK) {
            return err;
        }
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_RTC_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   /* R4 10k 外部上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    uint8_t ctrl2 = 0;
    esp_err_t err = ft_i2c_read_reg8(&s_rtc, RTC_REG_CTRL2, &ctrl2);
    if (err != ESP_OK) {
        return err;
    }
    ft_i2c_write_reg8(&s_rtc, RTC_REG_CTRL2, (uint8_t)(ctrl2 & ~(RTC_CTRL2_TF | RTC_CTRL2_AIE)));
    ft_i2c_write_reg8(&s_rtc, RTC_REG_TIMER_VALUE, 1);
    err = ft_i2c_write_reg8(&s_rtc, RTC_REG_TIMER_MODE,
                            RTC_TIMER_TIE | RTC_TIMER_TE | RTC_TIMER_TCF_1HZ);
    if (err != ESP_OK) {
        return err;
    }

    bool fired = false;
    for (int i = 0; i < 250; i++) {
        if (gpio_get_level(GPIO_RTC_INT) == 0) {
            fired = true;
            break;
        }
        ft_delay_ms(10);
    }

    ft_i2c_write_reg8(&s_rtc, RTC_REG_TIMER_MODE, 0x00);
    ft_i2c_read_reg8(&s_rtc, RTC_REG_CTRL2, &ctrl2);
    ft_i2c_write_reg8(&s_rtc, RTC_REG_CTRL2, (uint8_t)(ctrl2 & ~RTC_CTRL2_TF));
    return fired ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t ft_sensors_init(void)
{
    esp_err_t imu_err = ft_imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed: %s", esp_err_to_name(imu_err));
    }
    return imu_err;
}

