/**
 * @file  ft_pmic.h
 * @brief AXP2101 最小驱动：电源轨使能/电压、ADC、关机
 */
#pragma once

#include "ft_board.h"
#include "ft_i2c.h"

typedef enum {
    FT_RAIL_VCC = 0,    /* DC1  -> 3V3 主电源（ESP32 / Flash / I2C 上拉） */
    FT_RAIL_AVCC,       /* ALDO3 -> 传感器 / 麦克风 */
    FT_RAIL_VAUDIO,     /* DC3  -> MAX98357A 功放 */
    FT_RAIL_MAX,
} ft_pmic_rail_t;

esp_err_t ft_pmic_init(void);
int ft_pmic_chip_id(void);

/* PWRON is wired to PMIC_INT (GPIO41).  Events are latched by AXP2101 and
 * cleared by reading INTSTS1/2/3.  Enable only positive/negative edges. */
esp_err_t ft_pmic_irq_init(void);
bool ft_pmic_irq_available(void);
bool ft_pmic_irq_pending(void);
esp_err_t ft_pmic_read_irq(uint8_t st[3]);
esp_err_t ft_pmic_clear_irq(void);
bool ft_pmic_pwr_down_irq(const uint8_t st[3]);
bool ft_pmic_pwr_up_irq(const uint8_t st[3]);
void ft_pmic_print_irq(void);
esp_err_t ft_pmic_set_voltage(ft_pmic_rail_t rail, uint16_t mv);
int ft_pmic_get_voltage(ft_pmic_rail_t rail);
esp_err_t ft_pmic_rail_enable(ft_pmic_rail_t rail, bool on);
bool ft_pmic_rail_is_on(ft_pmic_rail_t rail);

int ft_pmic_read_batt_mv(void);
int ft_pmic_read_vsys_mv(void);
int ft_pmic_read_vbus_mv(void);
int ft_pmic_battery_percent(void);
bool ft_pmic_battery_present(void);
bool ft_pmic_vbus_present(void);

void ft_pmic_print_info(void);
esp_err_t ft_pmic_poweroff(void);

const char *ft_pmic_rail_name(ft_pmic_rail_t rail);
