/**
 * @file  ft_i2c.c
 * @brief 两路 I2C 总线（传感器 / 触摸）初始化与通用读写
 */
#include "ft_i2c.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "FT_I2C";

static i2c_master_bus_handle_t s_bus_sensor;
static i2c_master_bus_handle_t s_bus_touch;

static esp_err_t bus_create(i2c_port_num_t port, gpio_num_t sda, gpio_num_t scl,
                            i2c_master_bus_handle_t *out)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = port,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 0,
        .flags.allow_pd = 0,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus(port %d) failed: %s", (int)port, esp_err_to_name(err));
    }
    return err;
}

esp_err_t ft_i2c_init(void)
{
    if (s_bus_sensor && s_bus_touch) {
        return ESP_OK;
    }
    esp_err_t err = bus_create(FT_I2C_SENSOR_PORT, GPIO_IIC_SDA, GPIO_IIC_SCL, &s_bus_sensor);
    if (err != ESP_OK) {
        return err;
    }
    err = bus_create(FT_I2C_TOUCH_PORT, GPIO_LCD_TOUCH_SDA, GPIO_LCD_TOUCH_SCL, &s_bus_touch);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "sensor bus SDA=%d SCL=%d, touch bus SDA=%d SCL=%d",
             GPIO_IIC_SDA, GPIO_IIC_SCL, GPIO_LCD_TOUCH_SDA, GPIO_LCD_TOUCH_SCL);
    return ESP_OK;
}

void ft_i2c_deinit(void)
{
    /* 设备的生命周期与总线一致，这里不做释放，避免误删在用句柄 */
}

i2c_master_bus_handle_t ft_i2c_bus(bool touch_bus)
{
    return touch_bus ? s_bus_touch : s_bus_sensor;
}

esp_err_t ft_i2c_add(ft_i2c_dev_t *out, uint16_t addr, bool touch_bus)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_bus_handle_t bus = ft_i2c_bus(touch_bus);
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = FT_I2C_FREQ_HZ,
        .scl_wait_us = 0,
        .flags.disable_ack_check = 0,
    };
    memset(out, 0, sizeof(*out));
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &out->dev);
    if (err == ESP_OK) {
        out->addr = addr;
        out->touch_bus = touch_bus;
    }
    return err;
}

esp_err_t ft_i2c_del(ft_i2c_dev_t *dev)
{
    if (!dev || !dev->dev) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_master_bus_rm_device(dev->dev);
    if (err == ESP_OK) {
        memset(dev, 0, sizeof(*dev));
    }
    return err;
}

esp_err_t ft_i2c_read(ft_i2c_dev_t *dev, uint8_t *buf, size_t len)
{
    if (!dev || !dev->dev || (!buf && len)) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_receive(dev->dev, buf, len, FT_I2C_TIMEOUT_MS);
}

esp_err_t ft_i2c_write(ft_i2c_dev_t *dev, const uint8_t *buf, size_t len)
{
    if (!dev || !dev->dev || (!buf && len)) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit(dev->dev, buf, len, FT_I2C_TIMEOUT_MS);
}

esp_err_t ft_i2c_read_reg(ft_i2c_dev_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!dev || !dev->dev || (!buf && len)) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(dev->dev, &reg, 1, buf, len, FT_I2C_TIMEOUT_MS);
}

esp_err_t ft_i2c_write_reg(ft_i2c_dev_t *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    if (!dev || !dev->dev || (!buf && len) || len > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tmp[33];
    tmp[0] = reg;
    if (len) {
        memcpy(&tmp[1], buf, len);
    }
    return i2c_master_transmit(dev->dev, tmp, len + 1, FT_I2C_TIMEOUT_MS);
}

esp_err_t ft_i2c_read_reg8(ft_i2c_dev_t *dev, uint8_t reg, uint8_t *val)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }
    return ft_i2c_read_reg(dev, reg, val, 1);
}

esp_err_t ft_i2c_write_reg8(ft_i2c_dev_t *dev, uint8_t reg, uint8_t val)
{
    return ft_i2c_write_reg(dev, reg, &val, 1);
}

bool ft_i2c_probe(uint16_t addr, bool touch_bus)
{
    i2c_master_bus_handle_t bus = ft_i2c_bus(touch_bus);
    if (!bus) {
        return false;
    }
    return i2c_master_probe(bus, addr, FT_I2C_TIMEOUT_MS) == ESP_OK;
}

const char *ft_i2c_addr_name(uint16_t addr)
{
    switch (addr) {
    case FT_I2C_ADDR_AXP2101:  return "AXP2101 PMIC";
    case FT_I2C_ADDR_QMC5883P: return "QMC5883P MAG";
    case FT_I2C_ADDR_AHT20:    return "AHT20 T/H";
    case FT_I2C_ADDR_PCF85063: return "PCF85063 RTC";
    case FT_I2C_ADDR_CST816D:  return "CST816D TOUCH";
    default:                    return "unknown";
    }
}

int ft_i2c_scan(bool touch_bus)
{
    int found = 0;
    printf("  I2C %s bus:", touch_bus ? "touch" : "sensor");
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (ft_i2c_probe(addr, touch_bus)) {
            printf(" 0x%02X(%s)", addr, ft_i2c_addr_name(addr));
            found++;
        }
    }
    if (!found) {
        printf(" <none>");
    }
    printf("\n");
    return found;
}
