/**
 * @file  ft_pmic.c
 * @brief AXP2101 电源管理：上电配置 + 运行时轨道开关
 *
 * 关键寄存器（AXP2101 数据手册 / XPowersLib）：
 *   0x00 STATUS1 : bit3 电池接入, bit5 VBUS good
 *   0x01 STATUS2 : bit3 VBUS in(低有效), bit2:0 充电状态
 *   0x03 IC_TYPE : 0x4A
 *   0x10 COMMON_CONFIG : bit0 关机
 *   0x30 ADC_CHANNEL_CTRL : bit0 VBAT, bit3 VSYS
 *   0x34/0x35 VBAT(13bit), 0x38/0x39 VBUS(14bit), 0x3A/0x3B VSYS(14bit)
 *   0x80 DC_ONOFF : bit0 DC1, bit2 DC3
 *   0x82 DC1 电压 : (mV-1500)/100
 *   0x84 DC3 电压 : 0.5~1.2V 10mV/step, 1.22~1.54V 20mV/step, 1.6~3.4V 100mV/step
 *   0x90 LDO_ONOFF : bit2 ALDO3
 *   0x94 ALDO3 电压 : (mV-500)/100
 *   0xA4 电池百分比
 */
#include "ft_pmic.h"

static const char *TAG = "FT_PMIC";

#define AXP_REG_STATUS1      0x00
#define AXP_REG_STATUS2      0x01
#define AXP_REG_IC_TYPE      0x03
#define AXP_REG_COMMON_CFG   0x10
#define AXP_REG_PWROFF_EN    0x22
#define AXP_REG_OFF_LEVEL    0x27
#define AXP_REG_ADC_CH       0x30
#define AXP_REG_VBAT_H       0x34
#define AXP_REG_VBAT_L       0x35
#define AXP_REG_VBUS_H       0x38
#define AXP_REG_VBUS_L       0x39
#define AXP_REG_VSYS_H       0x3A
#define AXP_REG_VSYS_L       0x3B
#define AXP_REG_DC_ONOFF     0x80
#define AXP_REG_DC1_VOL      0x82
#define AXP_REG_DC3_VOL      0x84
#define AXP_REG_LDO_ONOFF    0x90
#define AXP_REG_ALDO3_VOL    0x94
#define AXP_REG_BAT_PERCENT  0xA4
#define AXP_REG_INTEN2       0x41
#define AXP_REG_INTSTS1      0x48
#define AXP_REG_INTSTS2      0x49
#define AXP_REG_INTSTS3      0x4A

#define AXP_INT2_PWR_UP      0x01
#define AXP_INT2_PWR_DOWN    0x02

#define AXP_DC1_ON_BIT       0x01
#define AXP_DC3_ON_BIT       0x04
#define AXP_ALDO3_ON_BIT     0x04

static ft_i2c_dev_t s_pmic;
static bool s_present;
static bool s_irq_ready;

static esp_err_t rd8(uint8_t reg, uint8_t *val)
{
    return ft_i2c_read_reg8(&s_pmic, reg, val);
}

static esp_err_t wr8(uint8_t reg, uint8_t val)
{
    return ft_i2c_write_reg8(&s_pmic, reg, val);
}

static esp_err_t update8(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t v = 0;
    esp_err_t err = rd8(reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    v = (uint8_t)((v & ~mask) | (val & mask));
    return wr8(reg, v);
}

static int read_h5l8(uint8_t regh, uint8_t regl)
{
    uint8_t b[2] = {0};
    if (ft_i2c_read_reg(&s_pmic, regh, b, 2) != ESP_OK) {
        return -1;
    }
    return ((b[0] & 0x1F) << 8) | b[1];
}

static int read_h6l8(uint8_t regh, uint8_t regl)
{
    uint8_t b[2] = {0};
    if (ft_i2c_read_reg(&s_pmic, regh, b, 2) != ESP_OK) {
        return -1;
    }
    return ((b[0] & 0x3F) << 8) | b[1];
}

/* ---------------- 电压码换算 ---------------- */
static int dc1_code_from_mv(int mv)
{
    if (mv < 1500 || mv > 3400 || (mv % 100)) {
        return -1;
    }
    return (mv - 1500) / 100;
}

static int dc1_mv_from_code(int code)
{
    return 1500 + (code & 0x7F) * 100;
}

static int aldo3_code_from_mv(int mv)
{
    if (mv < 500 || mv > 3500 || (mv % 100)) {
        return -1;
    }
    return (mv - 500) / 100;
}

static int aldo3_mv_from_code(int code)
{
    return 500 + (code & 0x3F) * 100;
}

static int dc3_code_from_mv(int mv)
{
    if (mv >= 500 && mv <= 1200 && (mv % 10) == 0) {
        return (mv - 500) / 10;
    }
    if (mv >= 1220 && mv <= 1540 && (mv % 20) == 0) {
        return 71 + (mv - 1220) / 20;
    }
    if (mv >= 1600 && mv <= 3400 && (mv % 100) == 0) {
        return 88 + (mv - 1600) / 100;
    }
    return -1;
}

static int dc3_mv_from_code(int code)
{
    code &= 0x7F;
    if (code <= 70) {
        return 500 + code * 10;
    }
    if (code <= 87) {
        return 1220 + (code - 71) * 20;
    }
    return 1600 + (code - 88) * 100;
}

static uint8_t rail_on_mask(ft_pmic_rail_t rail)
{
    switch (rail) {
    case FT_RAIL_VCC:    return AXP_DC1_ON_BIT;
    case FT_RAIL_VAUDIO: return AXP_DC3_ON_BIT;
    case FT_RAIL_AVCC:   return AXP_ALDO3_ON_BIT;
    default:             return 0;
    }
}

static uint8_t rail_on_reg(ft_pmic_rail_t rail)
{
    return (rail == FT_RAIL_AVCC) ? AXP_REG_LDO_ONOFF : AXP_REG_DC_ONOFF;
}

static uint8_t rail_vol_reg(ft_pmic_rail_t rail)
{
    switch (rail) {
    case FT_RAIL_VCC:    return AXP_REG_DC1_VOL;
    case FT_RAIL_VAUDIO: return AXP_REG_DC3_VOL;
    case FT_RAIL_AVCC:   return AXP_REG_ALDO3_VOL;
    default:             return 0;
    }
}

const char *ft_pmic_rail_name(ft_pmic_rail_t rail)
{
    switch (rail) {
    case FT_RAIL_VCC:    return "VCC(DC1)";
    case FT_RAIL_AVCC:   return "AVCC(ALDO3)";
    case FT_RAIL_VAUDIO: return "VAUDIO(DC3)";
    default:             return "?";
    }
}

esp_err_t ft_pmic_set_voltage(ft_pmic_rail_t rail, uint16_t mv)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t reg = rail_vol_reg(rail);
    int code = -1;
    switch (rail) {
    case FT_RAIL_VCC:    code = dc1_code_from_mv(mv); break;
    case FT_RAIL_VAUDIO: code = dc3_code_from_mv(mv); break;
    case FT_RAIL_AVCC:   code = aldo3_code_from_mv(mv); break;
    default: break;
    }
    if (code < 0) {
        ESP_LOGE(TAG, "%s 不支持 %u mV（步进/范围不匹配）", ft_pmic_rail_name(rail), mv);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t old = 0;
    esp_err_t err = rd8(reg, &old);
    if (err != ESP_OK) {
        return err;
    }
    /* 保留 bit7（DC 工作模式），只改电压码 */
    uint8_t val = (uint8_t)((old & 0x80) | (code & 0x7F));
    err = wr8(reg, val);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s -> %u mV (reg 0x%02X=0x%02X)", ft_pmic_rail_name(rail), mv, reg, val);
    }
    return err;
}

int ft_pmic_get_voltage(ft_pmic_rail_t rail)
{
    if (!s_present) {
        return -1;
    }
    uint8_t v = 0;
    if (rd8(rail_vol_reg(rail), &v) != ESP_OK) {
        return -1;
    }
    switch (rail) {
    case FT_RAIL_VCC:    return dc1_mv_from_code(v);
    case FT_RAIL_VAUDIO: return dc3_mv_from_code(v);
    case FT_RAIL_AVCC:   return aldo3_mv_from_code(v);
    default:             return -1;
    }
}

esp_err_t ft_pmic_rail_enable(ft_pmic_rail_t rail, bool on)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t mask = rail_on_mask(rail);
    if (!mask) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = update8(rail_on_reg(rail), mask, on ? mask : 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s %s", ft_pmic_rail_name(rail), on ? "ON" : "OFF");
    }
    return err;
}

bool ft_pmic_rail_is_on(ft_pmic_rail_t rail)
{
    uint8_t v = 0;
    if (!s_present || rd8(rail_on_reg(rail), &v) != ESP_OK) {
        return false;
    }
    return (v & rail_on_mask(rail)) != 0;
}

int ft_pmic_read_batt_mv(void)
{
    return s_present ? read_h5l8(AXP_REG_VBAT_H, AXP_REG_VBAT_L) : -1;
}

int ft_pmic_read_vsys_mv(void)
{
    return s_present ? read_h6l8(AXP_REG_VSYS_H, AXP_REG_VSYS_L) : -1;
}

int ft_pmic_read_vbus_mv(void)
{
    return s_present ? read_h6l8(AXP_REG_VBUS_H, AXP_REG_VBUS_L) : -1;
}

int ft_pmic_battery_percent(void)
{
    uint8_t v = 0;
    if (!s_present || rd8(AXP_REG_BAT_PERCENT, &v) != ESP_OK) {
        return -1;
    }
    return (v > 100) ? -1 : v;
}

bool ft_pmic_battery_present(void)
{
    uint8_t v = 0;
    return s_present && rd8(AXP_REG_STATUS1, &v) == ESP_OK && (v & 0x08);
}

bool ft_pmic_vbus_present(void)
{
    uint8_t s1 = 0, s2 = 0;
    if (!s_present || rd8(AXP_REG_STATUS1, &s1) != ESP_OK || rd8(AXP_REG_STATUS2, &s2) != ESP_OK) {
        return false;
    }
    return ((s1 & 0x20) != 0) && ((s2 & 0x08) == 0);
}

static const char *charger_state_str(uint8_t s2)
{
    switch (s2 & 0x07) {
    case 0: return "tri-charge";
    case 1: return "pre-charge";
    case 2: return "constant-current";
    case 3: return "constant-voltage";
    case 4: return "done";
    default: return "not-charging";
    }
}


void ft_pmic_print_info(void)
{
    if (!s_present) {
        printf("  PMIC: AXP2101 not found (addr 0x%02X)\n", FT_I2C_ADDR_AXP2101);
        return;
    }
    uint8_t id = 0, s1 = 0, s2 = 0, dc = 0, ldo = 0, adc = 0;
    rd8(AXP_REG_IC_TYPE, &id);
    rd8(AXP_REG_STATUS1, &s1);
    rd8(AXP_REG_STATUS2, &s2);
    rd8(AXP_REG_DC_ONOFF, &dc);
    rd8(AXP_REG_LDO_ONOFF, &ldo);
    rd8(AXP_REG_ADC_CH, &adc);

    printf("  AXP2101 ID=0x%02X (expect 0x4A)  STATUS1=0x%02X STATUS2=0x%02X\n", id, s1, s2);
    printf("  battery: %s  VBUS: %s  charger: %s  batt: %d%%\n",
           (s1 & 0x08) ? "present" : "absent",
           ft_pmic_vbus_present() ? "present" : "absent",
           charger_state_str(s2), ft_pmic_battery_percent());
    printf("  VBAT=%d mV%s  VSYS=%d mV  VBUS=%d mV  ADC_EN=0x%02X\n",
           ft_pmic_read_batt_mv(), (s1 & 0x08) ? "" : " (floating, no battery)",
           ft_pmic_read_vsys_mv(), ft_pmic_read_vbus_mv(), adc);
    printf("  DC_ONOFF=0x%02X  LDO_ONOFF=0x%02X\n", dc, ldo);
    for (int i = 0; i < FT_RAIL_MAX; i++) {
        printf("    %-12s %-3s %d mV\n", ft_pmic_rail_name((ft_pmic_rail_t)i),
               ft_pmic_rail_is_on((ft_pmic_rail_t)i) ? "ON" : "OFF",
               ft_pmic_get_voltage((ft_pmic_rail_t)i));
    }
}

int ft_pmic_chip_id(void)
{
    uint8_t id = 0;
    if (!s_present || rd8(AXP_REG_IC_TYPE, &id) != ESP_OK) {
        return -1;
    }
    return id;
}

esp_err_t ft_pmic_read_irq(uint8_t st[3])
{
    if (!s_present || !st) {
        return ESP_ERR_INVALID_STATE;
    }
    return ft_i2c_read_reg(&s_pmic, AXP_REG_INTSTS1, st, 3);
}

esp_err_t ft_pmic_clear_irq(void)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t ff[3] = {0xFF, 0xFF, 0xFF};
    return ft_i2c_write_reg(&s_pmic, AXP_REG_INTSTS1, ff, sizeof(ff));
}

bool ft_pmic_pwr_down_irq(const uint8_t st[3])
{
    return st && (st[1] & AXP_INT2_PWR_DOWN);
}

bool ft_pmic_pwr_up_irq(const uint8_t st[3])
{
    return st && (st[1] & AXP_INT2_PWR_UP);
}

bool ft_pmic_irq_available(void)
{
    return s_present && s_irq_ready;
}

bool ft_pmic_irq_pending(void)
{
    return s_irq_ready && gpio_get_level(GPIO_PMIC_INT) == 0;
}

esp_err_t ft_pmic_irq_init(void)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_PMIC_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   /* R6 10k external pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMIC_INT gpio config: %s", esp_err_to_name(err));
        return err;
    }

    /* Only PWRON positive/negative edges.  AXP latches the status until it is
     * read/cleared, so polling GPIO41 from the UI is enough. */
    err = wr8(AXP_REG_INTEN2, AXP_INT2_PWR_UP | AXP_INT2_PWR_DOWN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "INTEN2 write: %s", esp_err_to_name(err));
        return err;
    }
    ft_pmic_clear_irq();

    /* Long press (6 s) performs a hard PMIC power-off.  Short press stays a
     * normal UI key handled by the edge IRQs. */
    update8(AXP_REG_PWROFF_EN, 0x03, 0x02);
    update8(AXP_REG_OFF_LEVEL, 0x0C, 0x04);

    uint8_t inten = 0, pwroff = 0, level = 0;
    rd8(AXP_REG_INTEN2, &inten);
    rd8(AXP_REG_PWROFF_EN, &pwroff);
    rd8(AXP_REG_OFF_LEVEL, &level);
    s_irq_ready = true;
    ESP_LOGI(TAG, "IRQ ready: INTEN2=0x%02X PWROFF_EN=0x%02X OFF_LEVEL=0x%02X",
             inten, pwroff, level);
    return ESP_OK;
}

void ft_pmic_print_irq(void)
{
    uint8_t st[3] = {0};
    uint8_t inten = 0;
    if (!s_present) {
        printf("  PMIC IRQ: AXP2101 not present\n");
        return;
    }
    rd8(AXP_REG_INTEN2, &inten);
    if (ft_pmic_read_irq(st) == ESP_OK) {
        printf("  PMIC IRQ: GPIO%d=%d INTEN2=0x%02X INTSTS1/2/3=%02X %02X %02X"
               " pwr_down=%d pwr_up=%d\n",
               GPIO_PMIC_INT, gpio_get_level(GPIO_PMIC_INT), inten,
               st[0], st[1], st[2], ft_pmic_pwr_down_irq(st), ft_pmic_pwr_up_irq(st));
        ft_pmic_clear_irq();
    } else {
        printf("  PMIC IRQ: GPIO%d=%d INTEN2=0x%02X (read failed)\n",
               GPIO_PMIC_INT, gpio_get_level(GPIO_PMIC_INT), inten);
    }
}

esp_err_t ft_pmic_init(void)
{
    esp_err_t err = ESP_OK;
    if (!s_pmic.dev) {
        err = ft_i2c_add(&s_pmic, FT_I2C_ADDR_AXP2101, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "add AXP2101 failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    uint8_t id = 0;
    err = rd8(AXP_REG_IC_TYPE, &id);
    if (err != ESP_OK || id != 0x4A) {
        ESP_LOGE(TAG, "AXP2101 not detected (id=0x%02X, err=%s)", id, esp_err_to_name(err));
        s_present = false;
        return ESP_ERR_NOT_FOUND;
    }
    s_present = true;

    /* 先保证电压，再打开输出；VCC 已在运行，AVCC/VAUDIO 需要显式开启 */
    ft_pmic_set_voltage(FT_RAIL_VCC, 3300);
    ft_pmic_rail_enable(FT_RAIL_VCC, true);

    ft_pmic_set_voltage(FT_RAIL_AVCC, 3300);
    ft_pmic_rail_enable(FT_RAIL_AVCC, true);

    ft_pmic_set_voltage(FT_RAIL_VAUDIO, 3300);
    ft_pmic_rail_enable(FT_RAIL_VAUDIO, true);

    /* 使能 VBAT / VBUS / VSYS ADC（bit0/bit2/bit3，保留 bit1 TS） */
    update8(AXP_REG_ADC_CH, 0x0D, 0x0D);

    /* 给传感器/麦克风/功放电源域稳定时间 */
    ft_delay_ms(50);

    esp_err_t irq_err = ft_pmic_irq_init();
    if (irq_err != ESP_OK) {
        ESP_LOGW(TAG, "PMIC IRQ init failed: %s", esp_err_to_name(irq_err));
    }

    ESP_LOGI(TAG, "AXP2101 ready: VCC %d mV, AVCC %d mV, VAUDIO %d mV",
             ft_pmic_get_voltage(FT_RAIL_VCC), ft_pmic_get_voltage(FT_RAIL_AVCC),
             ft_pmic_get_voltage(FT_RAIL_VAUDIO));
    return ESP_OK;
}

esp_err_t ft_pmic_poweroff(void)
{
    if (!s_present) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = update8(AXP_REG_COMMON_CFG, 0x01, 0x01);
    ft_delay_ms(500);
    return err;
}
