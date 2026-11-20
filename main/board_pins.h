/**
 * @file    board_pins.h
 * @brief   ESP_Watch 板级引脚映射（ESP32-S3R8 / QFN-56）
 *
 * 数据来源
 * --------
 *   Doc/Netlist_Schematic1_2026-11-18.enet
 *   （嘉立创 EDA 导出网表，取 components[].pinInfoMap[].net 字段）
 *
 * 命名约定
 * --------
 *   - 宏名 = "GPIO_" + 网表中的网络名，可直接用原理图/网表里的网络名反查。
 *   - 宏值 = ESP-IDF 的 GPIO_NUM_x（数值是 GPIO 编号，**不是** QFN 封装引脚号）。
 *   - 每个宏后面的注释给出对应的 U2 封装引脚号与器件引脚，便于对照。
 *
 * 注意事项
 * --------
 *   - LCD_SDA / LCD_SCL 是显示屏的 4 线 SPI（MOSI / CLK），不是 I2C。
 *   - LCD_BLK 经 Q13（2N7002K，低边开关）驱动背光：高电平 = 背光点亮。
 *   - KEY_LEFT / KEY_RIGHT 没有外部上拉，使用前必须在软件里打开内部上拉。
 *   - GPIO33 ~ GPIO37 被 ESP32-S3R8 片内八线 PSRAM 占用，禁止使用。
 *   - GPIO26 / GPIO47 / GPIO48 本设计未连接（保留）。
 *   - 本文件只做"网络名 → GPIO 编号"翻译，不包含任何初始化逻辑。
 */
#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "board_pins.h 仅适用于 ESP32-S3 目标（ESP32-S3R8）"
#endif

/** 本映射表对应的网表文件（相对工程根目录） */
#define BOARD_PINMAP_NETLIST "Doc/Netlist_Schematic1_2026-11-18.enet"

/* ======================================================================
 * 传感器 SPI —— ICM-42670-P (U3)  4 线 SPI
 * ==================================================================== */
#define GPIO_IMU_SCK            GPIO_NUM_3   /**< U2 pin8  / U3 pin13 AP_SCL/AP_SCLK */
#define GPIO_IMU_MOSI           GPIO_NUM_2   /**< U2 pin7  / U3 pin14 AP_SDA/AP_SDIO/AP_SDI */
#define GPIO_IMU_MISO           GPIO_NUM_1   /**< U2 pin6  / U3 pin1  AP_SDO/AP_AD0 */
#define GPIO_IMU_CS             GPIO_NUM_4   /**< U2 pin9  / U3 pin12 AP_CS */

/* ======================================================================
 * 传感器 I2C —— QMC5883P(U4) / AHT20(U6) / PCF85063(U11) / AXP2101(U14)
 * 总线由 R8 / R9（4.7 kΩ）上拉到 VCC
 * ==================================================================== */
#define GPIO_IIC_SCL            GPIO_NUM_9   /**< U2 pin14 */
#define GPIO_IIC_SDA            GPIO_NUM_10  /**< U2 pin15 */

/* ======================================================================
 * 显示 SPI —— 经 CN1 24P FPC 连接屏（CLK / MOSI / CS / DC / RST / TE / BLK）
 * 注意：网表中的 LCD_SDA = SPI MOSI，LCD_SCL = SPI CLK
 * ==================================================================== */
#define GPIO_LCD_SCL            GPIO_NUM_17  /**< U2 pin23 / CN1 pin5  */
#define GPIO_LCD_SDA            GPIO_NUM_16  /**< U2 pin22 / CN1 pin7  (SPI MOSI) */
#define GPIO_LCD_CS             GPIO_NUM_18  /**< U2 pin24 / CN1 pin4  */
#define GPIO_LCD_DC             GPIO_NUM_11  /**< U2 pin16 / CN1 pin24 */
#define GPIO_LCD_RST            GPIO_NUM_38  /**< U2 pin43 / CN1 pin1  */
#define GPIO_LCD_TE             GPIO_NUM_21  /**< U2 pin27 / CN1 pin3  (撕裂同步输入) */
#define GPIO_LCD_BLK            GPIO_NUM_39  /**< U2 pin44 -> U13 G；高电平 = 背光开 */

/* ======================================================================
 * 触摸屏 I2C —— 经 CN1 连接
 * 总线由 R94 / R95（4.7 kΩ）上拉到 VCC
 * ==================================================================== */
#define GPIO_LCD_TOUCH_SCL      GPIO_NUM_12  /**< U2 pin17 / CN1 pin20 */
#define GPIO_LCD_TOUCH_SDA      GPIO_NUM_13  /**< U2 pin18 / CN1 pin19 */
#define GPIO_LCD_TOUCH_RST      GPIO_NUM_14  /**< U2 pin19 / CN1 pin18 */
#define GPIO_LCD_TOUCH_INT      GPIO_NUM_15  /**< U2 pin21 / CN1 pin17 (XTAL_32K_P, RTC GPIO) */

/* ======================================================================
 * 音频 I2S —— MAX98357A(U10) 功放 + MSM261S4030H0R(U9) 麦克风共用一组时钟
 * ==================================================================== */
#define GPIO_I2S_SCK            GPIO_NUM_6   /**< U2 pin11 -> U10 pin16 BCLK / U9 pin6 SCK */
#define GPIO_I2S_WS             GPIO_NUM_8   /**< U2 pin13 -> U10 pin14 LRCLK / U9 pin3 WS */
#define GPIO_I2S_DIN            GPIO_NUM_5   /**< U2 pin10 -> U10 pin1  DIN (ESP32 输出) */
#define GPIO_I2S_SD             GPIO_NUM_7   /**< U2 pin12 <- U9 pin7  SD  (ESP32 输入) */

/* ======================================================================
 * USB —— ESP32-S3 原生 USB，经 CH334P(U8) Hub 下行口 2
 * ==================================================================== */
#define GPIO_ESP_DM             GPIO_NUM_19  /**< U2 pin25 -> U8 pin7  DM2 (USB D-) */
#define GPIO_ESP_DP             GPIO_NUM_20  /**< U2 pin26 -> U8 pin8  DP2 (USB D+) */

/* ======================================================================
 * 调试串口 UART0 —— 经 CH343P(U1) 转 USB
 * ==================================================================== */
#define GPIO_ESP32_TX           GPIO_NUM_43  /**< U2 pin49 U0TXD -> U1 pin5 RXD */
#define GPIO_ESP32_RX           GPIO_NUM_44  /**< U2 pin50 U0RXD <- U1 pin4 TXD */

/* ======================================================================
 * 按键与中断
 * ==================================================================== */
#define GPIO_KEY_LEFT           GPIO_NUM_45  /**< U2 pin51 / SW3 pin1；需内部上拉，strapping 引脚 */
#define GPIO_KEY_RIGHT          GPIO_NUM_40  /**< U2 pin45 / SW3 pin2；需内部上拉 */
#define GPIO_BOOT_KEY           GPIO_NUM_0   /**< U2 pin5；经 R107(0Ω) 接 PWRON，电源键兼 BOOT 键 */
#define GPIO_PMIC_INT           GPIO_NUM_41  /**< U2 pin47 <- U14 pin38 IRQ；R6 10 kΩ 上拉 */
#define GPIO_RTC_INT            GPIO_NUM_42  /**< U2 pin48 <- U11 pin4 INT#；R4 10 kΩ 上拉 */

/* ======================================================================
 * 外部 NOR Flash —— W25Q128JVPIQ(U5)，走 SPI0/1 专用引脚
 * 一般不要把这些引脚复用为普通 GPIO
 * ==================================================================== */
#define GPIO_FLASH_SPI_HD       GPIO_NUM_27  /**< U2 pin30 SPIHD / U5 pin7 HOLD#/RESET#(IO3) */
#define GPIO_FLASH_SPI_WP       GPIO_NUM_28  /**< U2 pin31 SPIWP / U5 pin3 WP#(IO2) */
#define GPIO_FLASH_SPI_CS0      GPIO_NUM_29  /**< U2 pin32 SPICS0 / U5 pin1 CS# */
#define GPIO_FLASH_SPI_CLK      GPIO_NUM_30  /**< U2 pin33 SPICLK / U5 pin6 CLK */
#define GPIO_FLASH_SPI_Q        GPIO_NUM_31  /**< U2 pin34 SPIQ  / U5 pin2 DO(IO1) */
#define GPIO_FLASH_SPI_D        GPIO_NUM_32  /**< U2 pin35 SPID  / U5 pin5 DI(IO0) */

/* ======================================================================
 * Strapping / 保留
 * ==================================================================== */
#define GPIO_STRAP_BOOT_PD      GPIO_NUM_46  /**< U2 pin52；R10 100 kΩ 下拉，保证 SPI 启动；勿作他用 */

/* ======================================================================
 * 便于阅读的别名（左侧只在代码里更直观，网络名仍是右侧那个）
 * ==================================================================== */
#define GPIO_LCD_MOSI           GPIO_LCD_SDA      /**< = GPIO_LCD_SDA (GPIO16) */
#define GPIO_LCD_SCLK           GPIO_LCD_SCL      /**< = GPIO_LCD_SCL (GPIO17) */
#define GPIO_I2S_BCLK           GPIO_I2S_SCK      /**< = GPIO_I2S_SCK (GPIO6) */
#define GPIO_I2S_LRCLK          GPIO_I2S_WS       /**< = GPIO_I2S_WS  (GPIO8) */
#define GPIO_I2S_DOUT           GPIO_I2S_DIN      /**< = GPIO_I2S_DIN (GPIO5) */
#define GPIO_I2S_MIC_DATA       GPIO_I2S_SD       /**< = GPIO_I2S_SD  (GPIO7) */

/* ======================================================================
 * 本设计未使用 / 不可用的 GPIO（仅作记录，不提供宏）
 * ----------------------------------------------------------------------
 *   GPIO22 ~ GPIO25 : ESP32-S3 芯片本身不存在这些 GPIO
 *   GPIO26          : U2 pin28 SPICS1，未连接
 *   GPIO33 ~ GPIO37 : ESP32-S3R8 片内八线 PSRAM 占用（SPIIO4~7 / SPIDQS），禁止使用
 *   GPIO47 ~ GPIO48 : U2 pin36/pin37 SPICLK_N / SPICLK_P，未连接，建议保留
 *   非 GPIO 引脚    : pin1 LNA_IN、pin53/54 XTAL_N/P(40 MHz)、pin2/3 VDD3P3、
 *                     pin4 CHIP_PU、pin20 VDD3P3_RTC、pin29 VDD_SPI、
 *                     pin46 VDD3P3_CPU、pin55/56 VDDA、pin57 GND(EP)
 * ==================================================================== */
