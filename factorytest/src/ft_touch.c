/**
 * @file  ft_touch.c
 * @brief CST816D 触摸：探测、坐标/手势读取、复位
 *
 * 寄存器（CST816S/T/D 系列通用）：
 *   0x00 状态, 0x01 手势, 0x02 触点数,
 *   0x03/0x04 X 高/低, 0x05/0x06 Y 高/低,
 *   0xFE 关闭自动休眠, 0xE5 休眠, 0xA7 Chip ID (CST816D = 0xB6), 0xA9 固件版本
 */
#include "ft_touch.h"

static const char *TAG = "FT_TOUCH";

#define CST_REG_STATUS        0x00
#define CST_REG_GESTURE       0x01
#define CST_REG_POINTS        0x02
#define CST_REG_X_H           0x03
#define CST_REG_DIS_AUTOSLEEP 0xFE
#define CST_REG_CHIP_ID       0xA7
#define CST_REG_FW_VERSION    0xA9

static ft_i2c_dev_t s_touch;
static bool s_present;
static uint8_t s_chip_id;
static uint8_t s_fw;

void ft_touch_reset(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_LCD_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(GPIO_LCD_TOUCH_RST, 1);
    ft_delay_ms(10);
    gpio_set_level(GPIO_LCD_TOUCH_RST, 0);
    ft_delay_ms(30);
    gpio_set_level(GPIO_LCD_TOUCH_RST, 1);
    ft_delay_ms(60);
}

bool ft_touch_int_level(void)
{
    return gpio_get_level(GPIO_LCD_TOUCH_INT) != 0;
}

esp_err_t ft_touch_init(void)
{
    esp_err_t err = ft_i2c_add(&s_touch, FT_I2C_ADDR_CST816D, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add CST816D failed: %s", esp_err_to_name(err));
        return err;
    }
    /* INT 为输入；面板侧可能为开漏，使能内部上拉以防浮空 */
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << GPIO_LCD_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&int_cfg);

    ft_touch_reset();

    err = ft_i2c_read_reg8(&s_touch, CST_REG_CHIP_ID, &s_chip_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read chip id failed: %s", esp_err_to_name(err));
        s_present = false;
        return err;
    }
    ft_i2c_read_reg8(&s_touch, CST_REG_FW_VERSION, &s_fw);

    /* 工厂测试期间关闭自动休眠，保证随时可读 */
    uint8_t v = 0x01;
    ft_i2c_write_reg8(&s_touch, CST_REG_DIS_AUTOSLEEP, v);

    s_present = true;
    ESP_LOGI(TAG, "CST816D found, chip id=0x%02X fw=0x%02X", s_chip_id, s_fw);
    return ESP_OK;
}

bool ft_touch_present(void)
{
    return s_present;
}

uint8_t ft_touch_chip_id(void)
{
    return s_chip_id;
}

uint8_t ft_touch_fw_version(void)
{
    return s_fw;
}

esp_err_t ft_touch_read(ft_touch_data_t *data)
{
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[7] = {0};
    esp_err_t err = ft_i2c_read_reg(&s_touch, CST_REG_STATUS, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    data->status = b[0];
    data->gesture = b[1];
    data->points = b[2] & 0x0F;
    data->x = (uint16_t)(((b[3] & 0x0F) << 8) | b[4]);
    data->y = (uint16_t)(((b[5] & 0x0F) << 8) | b[6]);
    return ESP_OK;
}
