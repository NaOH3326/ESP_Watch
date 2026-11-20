/* Auto-generated panel init table (GC9307 only).
 * Source: gc9307-async-rs vendor sequence.
 */
#include "ft_lcd.h"

/* GC9307：240x284 IPS 厂商序列（参考 gc9307-async-rs，按 240x284 面板使用） */
const ft_lcd_cmd_t ft_lcd_gc9307_init[] = {
    {0xFE, 0, NULL, 0},
    {0xEF, 0, NULL, 0},
    {0x36, 1, (const uint8_t[]){0x48}, 0},
    {0x3A, 1, (const uint8_t[]){0x05}, 0},
    {0x85, 1, (const uint8_t[]){0xC0}, 0},
    {0x86, 1, (const uint8_t[]){0x98}, 0},
    {0x87, 1, (const uint8_t[]){0x28}, 0},
    {0x89, 1, (const uint8_t[]){0x33}, 0},
    {0x8B, 1, (const uint8_t[]){0x84}, 0},
    {0x8D, 1, (const uint8_t[]){0x3B}, 0},
    {0x8E, 1, (const uint8_t[]){0x0F}, 0},
    {0x8F, 1, (const uint8_t[]){0x70}, 0},
    {0xE8, 2, (const uint8_t[]){0x13, 0x17}, 0},
    {0xEC, 3, (const uint8_t[]){0x57, 0x07, 0xFF}, 0},
    {0xED, 2, (const uint8_t[]){0x18, 0x09}, 0},
    {0xC9, 1, (const uint8_t[]){0x10}, 0},
    {0xFF, 1, (const uint8_t[]){0x61}, 0},
    {0x99, 1, (const uint8_t[]){0x3A}, 0},
    {0x9D, 1, (const uint8_t[]){0x43}, 0},
    {0x98, 1, (const uint8_t[]){0x3E}, 0},
    {0x9C, 1, (const uint8_t[]){0x4B}, 0},
    {0xF0, 6, (const uint8_t[]){0x06, 0x08, 0x08, 0x06, 0x05, 0x1D}, 0},
    {0xF2, 6, (const uint8_t[]){0x00, 0x01, 0x09, 0x07, 0x04, 0x23}, 0},
    {0xF1, 6, (const uint8_t[]){0x3B, 0x68, 0x66, 0x36, 0x35, 0x2F}, 0},
    {0xF3, 6, (const uint8_t[]){0x37, 0x6A, 0x66, 0x37, 0x35, 0x35}, 0},
    {0xFA, 2, (const uint8_t[]){0x80, 0x0F}, 0},
    {0xBE, 1, (const uint8_t[]){0x11}, 0},
    {0xCB, 1, (const uint8_t[]){0x02}, 0},
    {0xCD, 1, (const uint8_t[]){0x22}, 0},
    {0x9B, 1, (const uint8_t[]){0xFF}, 0},
    {0x35, 1, (const uint8_t[]){0x00}, 0},
    {0x44, 2, (const uint8_t[]){0x00, 0x0A}, 0},
    {0x11, 0, NULL, 200},
    {0x29, 0, NULL, 50},
};
const size_t ft_lcd_gc9307_init_len = sizeof(ft_lcd_gc9307_init) / sizeof(ft_lcd_gc9307_init[0]);

