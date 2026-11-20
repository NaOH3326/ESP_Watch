/**
 * @file  ft_lcd.h
 * @brief ST77916 / NV3030B SPI 屏驱动（仅工厂测试用，最小实现）
 */
#pragma once

#include "ft_board.h"
#include "esp_lcd_panel_io.h"

typedef struct {
    uint8_t cmd;
    uint8_t len;
    const uint8_t *data;
    uint16_t delay_ms;
} ft_lcd_cmd_t;

extern const ft_lcd_cmd_t ft_lcd_gc9307_init[];
extern const size_t ft_lcd_gc9307_init_len;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t x_offset;
    uint16_t y_offset;
    uint8_t rotation;
    uint8_t brightness;
    uint8_t spi_mode;
    uint32_t spi_hz;
    bool initialized;
    bool have_fb;
} ft_lcd_info_t;

esp_err_t ft_lcd_init(void);
esp_err_t ft_lcd_reinit(void);
esp_err_t ft_lcd_set_resolution(uint16_t width, uint16_t height, uint16_t x_offset, uint16_t y_offset);
const char *ft_lcd_controller_name(void);
const ft_lcd_info_t *ft_lcd_info(void);
const uint16_t *ft_lcd_framebuffer(void);
size_t ft_lcd_framebuffer_size(void);

esp_err_t ft_lcd_flush(void);
esp_err_t ft_lcd_flush_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
esp_err_t ft_lcd_fill(uint16_t color);
esp_err_t ft_lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void ft_lcd_draw_pixel(int x, int y, uint16_t color);
void ft_lcd_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
void ft_lcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void ft_lcd_draw_text(uint16_t x, uint16_t y, const char *text, uint16_t fg, uint16_t bg, uint8_t scale);

esp_err_t ft_lcd_backlight(uint8_t percent);
esp_err_t ft_lcd_set_rotation(uint8_t rotation);
esp_err_t ft_lcd_set_spi(uint8_t mode, uint32_t mhz);

void ft_lcd_show_bars(void);
void ft_lcd_show_gradient(void);
void ft_lcd_show_grid(void);
void ft_lcd_show_pattern(void);
void ft_lcd_banner(const char *line1, const char *line2);
