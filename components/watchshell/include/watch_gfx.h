#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "watch_types.h"

esp_err_t watch_gfx_init(void);
int watch_gfx_width(void);
int watch_gfx_height(void);

void watch_gfx_clear(uint16_t color);
void watch_gfx_flush(void);
void watch_gfx_flush_rect(int x, int y, int w, int h);

void watch_gfx_pixel(int x, int y, uint16_t color);
void watch_gfx_line(int x0, int y0, int x1, int y1, uint16_t color);
void watch_gfx_hline(int x, int y, int w, uint16_t color);
void watch_gfx_vline(int x, int y, int h, uint16_t color);
void watch_gfx_rect(int x, int y, int w, int h, uint16_t color);
void watch_gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
void watch_gfx_circle(int cx, int cy, int r, uint16_t color);
void watch_gfx_fill_circle(int cx, int cy, int r, uint16_t color);
void watch_gfx_arc(int cx, int cy, int r, int start_deg, int end_deg, uint16_t color);
void watch_gfx_ring_dots(int cx, int cy, int r, int count, int active, uint16_t color);
void watch_gfx_scanlines(int y, int h, uint16_t color);
void watch_gfx_blit_scaled(int x, int y, int w, int h,
                           const uint16_t *src, int src_w, int src_h);

int watch_gfx_text_width(const char *text, int scale);
int watch_gfx_text_height(int scale);
void watch_gfx_char(int x, int y, char ch, uint16_t fg, int scale);
void watch_gfx_text(int x, int y, const char *text, uint16_t fg, int scale);
void watch_gfx_text_bg(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale);
void watch_gfx_text_center(int cx, int y, const char *text, uint16_t fg, int scale);
void watch_gfx_text_right(int right, int y, const char *text, uint16_t fg, int scale);
void watch_gfx_printf(int x, int y, uint16_t fg, int scale, const char *fmt, ...);

/* 14 px mixed ASCII/CJK text used by the chat UI.  GB2312 glyphs are packed
 * 1-bpp in flash; ASCII keeps using the existing 5x7 font. */
int watch_gfx_cjk_text_width(const char *text);
int watch_gfx_cjk_text_height(void);
void watch_gfx_cjk_text(int x, int y, const char *text, uint16_t fg);

uint16_t watch_gfx_color_name(const char *name);
const char *watch_gfx_color_label(uint16_t color);

/* Line-art icon used by the carousel and app headers. */
void watch_gfx_icon(ws_app_id_t app, int cx, int cy, int size, int angle_deg, uint16_t color);

/* Small helpers used by apps. */
void watch_gfx_battery(int x, int y, int percent, bool charging, uint16_t color);
void watch_gfx_wifi(int x, int y, int rssi, uint16_t color);
void watch_gfx_checkbox(int x, int y, bool on, uint16_t color);
