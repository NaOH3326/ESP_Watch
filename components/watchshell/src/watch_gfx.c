#include "watch_gfx.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ft_font5x7.h"
#include "ft_lcd.h"
#include "sdkconfig.h"
#include "watch_font_cjk.h"

#define PI_F 3.14159265f

static int s_w;
static int s_h;

static void glyph_draw(int x, int y, unsigned char ch, uint16_t fg, uint16_t bg, int scale, bool fill_bg)
{
    if (ch < FT_FONT_FIRST_CHAR || ch > FT_FONT_LAST_CHAR) {
        ch = ' ';
    }
    const unsigned char *glyph = &ft_font5x7[(ch - FT_FONT_FIRST_CHAR) * FT_FONT_WIDTH];
    for (int col = 0; col < FT_FONT_WIDTH; col++) {
        unsigned char bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            bool on = (bits & (1u << row)) != 0;
            if (!on && !fill_bg) {
                continue;
            }
            uint16_t c = on ? fg : bg;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    watch_gfx_pixel(x + col * scale + sx, y + row * scale + sy, c);
                }
            }
        }
    }
}

esp_err_t watch_gfx_init(void)
{
    esp_err_t err = ft_lcd_init();
    if (err != ESP_OK) {
        return err;
    }
    const ft_lcd_info_t *info = ft_lcd_info();
    s_w = info ? info->width : WS_SCREEN_W;
    s_h = info ? info->height : WS_SCREEN_H;
#if defined(CONFIG_WATCH_LCD_SPI_MHZ)
    if (CONFIG_WATCH_LCD_SPI_MHZ > 0 && CONFIG_WATCH_LCD_SPI_MHZ != 40) {
        ft_lcd_set_spi(0, CONFIG_WATCH_LCD_SPI_MHZ);
    }
#endif
    ft_lcd_fill_rect(0, 0, s_w, s_h, WS_COLOR_BLACK);
    ft_lcd_flush();
    return ESP_OK;
}

int watch_gfx_width(void) { return s_w; }
int watch_gfx_height(void) { return s_h; }

void watch_gfx_clear(uint16_t color)
{
    ft_lcd_fill_rect(0, 0, (uint16_t)s_w, (uint16_t)s_h, color);
}

void watch_gfx_flush(void) { ft_lcd_flush(); }

void watch_gfx_flush_rect(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    if (w <= 0 || h <= 0) return;
    ft_lcd_flush_region((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
}

void watch_gfx_pixel(int x, int y, uint16_t color)
{
    ft_lcd_draw_pixel(x, y, color);
}

void watch_gfx_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x0 == x1) {
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
        for (int y = y0; y <= y1; y++) watch_gfx_pixel(x0, y, color);
        return;
    }
    if (y0 == y1) {
        if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
        for (int x = x0; x <= x1; x++) watch_gfx_pixel(x, y0, color);
        return;
    }
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        watch_gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void watch_gfx_hline(int x, int y, int w, uint16_t color)
{
    watch_gfx_line(x, y, x + w - 1, y, color);
}

void watch_gfx_vline(int x, int y, int h, uint16_t color)
{
    watch_gfx_line(x, y, x, y + h - 1, color);
}

void watch_gfx_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    watch_gfx_hline(x, y, w, color);
    watch_gfx_hline(x, y + h - 1, w, color);
    watch_gfx_vline(x, y, h, color);
    watch_gfx_vline(x + w - 1, y, h, color);
}

void watch_gfx_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_w) w = s_w - x;
    if (y + h > s_h) h = s_h - y;
    if (w <= 0 || h <= 0) return;
    ft_lcd_fill_rect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h, color);
}

void watch_gfx_circle(int cx, int cy, int r, uint16_t color)
{
    int x = r, y = 0, err = 0;
    while (x >= y) {
        watch_gfx_pixel(cx + x, cy + y, color);
        watch_gfx_pixel(cx + y, cy + x, color);
        watch_gfx_pixel(cx - y, cy + x, color);
        watch_gfx_pixel(cx - x, cy + y, color);
        watch_gfx_pixel(cx - x, cy - y, color);
        watch_gfx_pixel(cx - y, cy - x, color);
        watch_gfx_pixel(cx + y, cy - x, color);
        watch_gfx_pixel(cx + x, cy - y, color);
        y++;
        if (err <= 0) err += 2 * y + 1;
        if (err > 0) { x--; err -= 2 * x + 1; }
    }
}

void watch_gfx_fill_circle(int cx, int cy, int r, uint16_t color)
{
    for (int y = -r; y <= r; y++) {
        int w = (int)sqrtf((float)(r * r - y * y));
        watch_gfx_hline(cx - w, cy + y, 2 * w + 1, color);
    }
}

void watch_gfx_arc(int cx, int cy, int r, int start_deg, int end_deg, uint16_t color)
{
    if (end_deg < start_deg) { int t = start_deg; start_deg = end_deg; end_deg = t; }
    int steps = (end_deg - start_deg) * 2;
    if (steps < 2) steps = 2;
    int lx = 0, ly = 0;
    for (int i = 0; i <= steps; i++) {
        float a = (start_deg + (end_deg - start_deg) * (float)i / steps) * PI_F / 180.0f;
        int x = cx + (int)(cosf(a) * r);
        int y = cy + (int)(sinf(a) * r);
        if (i) watch_gfx_line(lx, ly, x, y, color);
        lx = x; ly = y;
    }
}

void watch_gfx_ring_dots(int cx, int cy, int r, int count, int active, uint16_t color)
{
    for (int i = 0; i < count; i++) {
        float a = (-90.0f + i * (360.0f / count)) * PI_F / 180.0f;
        int x = cx + (int)(cosf(a) * r);
        int y = cy + (int)(sinf(a) * r);
        if (i == active) {
            watch_gfx_fill_rect(x - 1, y - 1, 4, 4, color);
        } else {
            watch_gfx_pixel(x, y, WS_COLOR_DIM);
        }
    }
}

void watch_gfx_scanlines(int y, int h, uint16_t color)
{
    for (int yy = y; yy < y + h; yy += 4) {
        for (int x = 0; x < s_w; x += 3) {
            watch_gfx_pixel(x, yy, color);
        }
    }
}

void watch_gfx_blit_scaled(int x, int y, int w, int h,
                           const uint16_t *src, int src_w, int src_h)
{
    if (!src || src_w <= 0 || src_h <= 0 || w <= 0 || h <= 0) return;
    if (w == src_w && h == src_h) {
        for (int dy = 0; dy < h; dy++) {
            const uint16_t *row = src + dy * src_w;
            for (int dx = 0; dx < w; dx++) {
                watch_gfx_pixel(x + dx, y + dy, row[dx]);
            }
        }
        return;
    }
    for (int dy = 0; dy < h; dy++) {
        int sy = dy * src_h / h;
        const uint16_t *row = src + sy * src_w;
        for (int dx = 0; dx < w; dx++) {
            int sx = dx * src_w / w;
            watch_gfx_pixel(x + dx, y + dy, row[sx]);
        }
    }
}

int watch_gfx_text_width(const char *text, int scale)
{
    if (!text) return 0;
    return (int)strlen(text) * (FT_FONT_WIDTH + 1) * scale;
}

int watch_gfx_text_height(int scale) { return (FT_FONT_HEIGHT + 1) * scale; }

void watch_gfx_char(int x, int y, char ch, uint16_t fg, int scale)
{
    glyph_draw(x, y, (unsigned char)ch, fg, WS_COLOR_BLACK, scale, false);
}

void watch_gfx_text(int x, int y, const char *text, uint16_t fg, int scale)
{
    if (!text || scale < 1) return;
    int cx = x;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '\n') {
            cx = x;
            y += watch_gfx_text_height(scale);
            continue;
        }
        glyph_draw(cx, y, *p, fg, WS_COLOR_BLACK, scale, false);
        cx += (FT_FONT_WIDTH + 1) * scale;
        if (cx + FT_FONT_WIDTH * scale > s_w) {
            cx = x;
            y += watch_gfx_text_height(scale);
        }
    }
}

void watch_gfx_text_bg(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    if (!text || scale < 1) return;
    int w = watch_gfx_text_width(text, scale);
    int h = watch_gfx_text_height(scale);
    watch_gfx_fill_rect(x, y, w, h, bg);
    int cx = x;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '\n') {
            cx = x;
            y += h;
            continue;
        }
        glyph_draw(cx, y, *p, fg, bg, scale, true);
        cx += (FT_FONT_WIDTH + 1) * scale;
    }
}

void watch_gfx_text_center(int cx, int y, const char *text, uint16_t fg, int scale)
{
    watch_gfx_text(cx - watch_gfx_text_width(text, scale) / 2, y, text, fg, scale);
}

void watch_gfx_text_right(int right, int y, const char *text, uint16_t fg, int scale)
{
    watch_gfx_text(right - watch_gfx_text_width(text, scale), y, text, fg, scale);
}

void watch_gfx_printf(int x, int y, uint16_t fg, int scale, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    watch_gfx_text(x, y, buf, fg, scale);
}

#define CJK_ASCII_W 6
#define CJK_GLYPH_W WATCH_FONT_CJK_SIZE
#define CJK_GLYPH_H WATCH_FONT_CJK_SIZE
#define CJK_ASCII_Y_OFFSET ((CJK_GLYPH_H - FT_FONT_HEIGHT) / 2)

static uint32_t utf8_next(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    if (!p || !*p) return 0;
    uint32_t cp;
    int extra;
    if (*p < 0x80) {
        cp = *p;
        extra = 0;
    } else if ((*p & 0xE0) == 0xC0) {
        cp = *p & 0x1F;
        extra = 1;
    } else if ((*p & 0xF0) == 0xE0) {
        cp = *p & 0x0F;
        extra = 2;
    } else if ((*p & 0xF8) == 0xF0) {
        cp = *p & 0x07;
        extra = 3;
    } else {
        (*text)++;
        return '?';
    }
    p++;
    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) {
            *text = (const char *)p;
            return '?';
        }
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    *text = (const char *)p;
    return cp;
}

static int cjk_glyph_index(uint32_t cp)
{
    int lo = 0;
    int hi = (int)WATCH_FONT_CJK_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t value = watch_font_cjk_codepoints[mid];
        if (value == cp) return mid;
        if (value < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int cjk_cp_width(uint32_t cp)
{
    return cp < 0x80 ? CJK_ASCII_W : CJK_GLYPH_W;
}

static void cjk_glyph_draw(int x, int y, uint32_t cp, uint16_t fg)
{
    if (cp < 0x80) {
        glyph_draw(x, y + CJK_ASCII_Y_OFFSET, (unsigned char)cp, fg,
                   WS_COLOR_BLACK, 1, false);
        return;
    }
    int index = cjk_glyph_index(cp);
    if (index < 0) {
        glyph_draw(x + 3, y + CJK_ASCII_Y_OFFSET, '?', fg,
                   WS_COLOR_BLACK, 1, false);
        return;
    }
    const uint8_t *glyph = &watch_font_cjk_bitmap[index * WATCH_FONT_CJK_GLYPH_BYTES];
    for (int row = 0; row < CJK_GLYPH_H; row++) {
        uint16_t bits = (uint16_t)((glyph[row * WATCH_FONT_CJK_ROW_BYTES] << 8) |
                                   glyph[row * WATCH_FONT_CJK_ROW_BYTES + 1]);
        for (int col = 0; col < CJK_GLYPH_W; col++) {
            if (bits & (1u << (15 - col))) {
                watch_gfx_pixel(x + col, y + row, fg);
            }
        }
    }
}

int watch_gfx_cjk_text_width(const char *text)
{
    if (!text) return 0;
    int width = 0;
    while (*text) {
        uint32_t cp = utf8_next(&text);
        if (cp == '\n') continue;
        width += cjk_cp_width(cp);
    }
    return width;
}

int watch_gfx_cjk_text_height(void)
{
    return CJK_GLYPH_H;
}

void watch_gfx_cjk_text(int x, int y, const char *text, uint16_t fg)
{
    if (!text) return;
    int cx = x;
    int cy = y;
    while (*text) {
        uint32_t cp = utf8_next(&text);
        if (cp == '\n') {
            cx = x;
            cy += CJK_GLYPH_H;
            continue;
        }
        cjk_glyph_draw(cx, cy, cp, fg);
        cx += cjk_cp_width(cp);
    }
}

uint16_t watch_gfx_color_name(const char *name)
{
    if (!name) return WS_COLOR_GREEN;
    if (!strcasecmp(name, "black")) return WS_COLOR_BLACK;
    if (!strcasecmp(name, "white")) return WS_COLOR_WHITE;
    if (!strcasecmp(name, "green") || !strcasecmp(name, "lime")) return WS_COLOR_GREEN;
    if (!strcasecmp(name, "dim") || !strcasecmp(name, "gray") || !strcasecmp(name, "grey")) return WS_COLOR_GRAY;
    if (!strcasecmp(name, "cyan")) return WS_COLOR_CYAN;
    if (!strcasecmp(name, "amber") || !strcasecmp(name, "yellow")) return WS_COLOR_AMBER;
    if (!strcasecmp(name, "red")) return WS_COLOR_RED;
    if (!strcasecmp(name, "blue")) return WS_COLOR_BLUE;
    if (!strcasecmp(name, "magenta") || !strcasecmp(name, "purple")) return WS_COLOR_MAGENTA;
    return WS_COLOR_GREEN;
}

const char *watch_gfx_color_label(uint16_t color)
{
    switch (color) {
    case WS_COLOR_WHITE: return "white";
    case WS_COLOR_CYAN: return "cyan";
    case WS_COLOR_AMBER: return "amber";
    case WS_COLOR_RED: return "red";
    case WS_COLOR_BLUE: return "blue";
    case WS_COLOR_MAGENTA: return "magenta";
    case WS_COLOR_GRAY: return "gray";
    default: return "green";
    }
}

typedef struct {
    int cx, cy, s;
    float sn, cs;
    uint16_t color;
} icon_ctx_t;

static void ipoint(icon_ctx_t *c, int x, int y, int *ox, int *oy)
{
    int px = x * c->s / 100;
    int py = y * c->s / 100;
    *ox = c->cx + (int)(px * c->cs - py * c->sn);
    *oy = c->cy + (int)(px * c->sn + py * c->cs);
}

static void iline(icon_ctx_t *c, int x0, int y0, int x1, int y1)
{
    int ax, ay, bx, by;
    ipoint(c, x0, y0, &ax, &ay);
    ipoint(c, x1, y1, &bx, &by);
    watch_gfx_line(ax, ay, bx, by, c->color);
}

static void icircle(icon_ctx_t *c, int r)
{
    watch_gfx_circle(c->cx, c->cy, r * c->s / 100, c->color);
}

static void iarc(icon_ctx_t *c, int r, int a0, int a1)
{
    watch_gfx_arc(c->cx, c->cy, r * c->s / 100, a0 + (int)(-atan2f(c->sn, c->cs) * 180.0f / PI_F),
                  a1 + (int)(-atan2f(c->sn, c->cs) * 180.0f / PI_F), c->color);
}

void watch_gfx_icon(ws_app_id_t app, int cx, int cy, int size, int angle_deg, uint16_t color)
{
    icon_ctx_t c = {
        .cx = cx, .cy = cy, .s = size,
        .sn = sinf(angle_deg * PI_F / 180.0f),
        .cs = cosf(angle_deg * PI_F / 180.0f),
        .color = color,
    };
    switch (app) {
    case WS_APP_WATCHFACE:
        icircle(&c, 46);
        icircle(&c, 40);
        iline(&c, 0, 0, 0, -28);
        iline(&c, 0, 0, 22, 10);
        watch_gfx_fill_circle(cx, cy, size / 22, color);
        for (int i = 0; i < 12; i++) {
            float a = i * 30.0f * PI_F / 180.0f;
            iline(&c, (int)(sinf(a) * 34), (int)(-cosf(a) * 34),
                  (int)(sinf(a) * 38), (int)(-cosf(a) * 38));
        }
        break;
    case WS_APP_AI:
        iarc(&c, 28, 200, 340);
        iarc(&c, 38, 205, 335);
        iarc(&c, 48, 210, 330);
        icircle(&c, 12);
        watch_gfx_fill_circle(cx, cy, size / 18, color);
        iline(&c, -52, 20, -38, 20);
        iline(&c, 38, 20, 52, 20);
        break;
    case WS_APP_SHELL:
        watch_gfx_rect(cx - size * 48 / 100, cy - size * 36 / 100,
                       size * 96 / 100, size * 72 / 100, color);
        iline(&c, -32, -18, -16, -4);
        iline(&c, -16, -4, -32, 10);
        iline(&c, -8, 12, 14, 12);
        iline(&c, 22, -20, 22, -8);
        iline(&c, 24, -14, 32, -14);
        break;
    case WS_APP_SENSORS:
        iline(&c, -44, 32, 0, 32);
        iline(&c, 0, 32, 0, -34);
        iline(&c, 0, 32, 38, 32);
        for (int x = -38; x <= 34; x += 2) {
            int y = (int)(sinf((x + 38) / 12.0f) * 14.0f);
            int y2 = (int)(sinf((x + 36) / 12.0f) * 14.0f);
            iline(&c, x, y, x + 2, y2);
        }
        watch_gfx_fill_circle(cx, cy + size * 32 / 100, size / 22, color);
        break;
    case WS_APP_MEDIA:
        iline(&c, -24, -30, -24, 20);
        iline(&c, -24, -30, 24, -38);
        iline(&c, 24, -38, 24, 12);
        watch_gfx_fill_circle(cx - size * 16 / 100, cy + size * 20 / 100, size / 12, color);
        watch_gfx_fill_circle(cx + size * 24 / 100, cy + size * 12 / 100, size / 12, color);
        iline(&c, 4, -10, 34, 0);
        iline(&c, 34, 0, 4, 10);
        iline(&c, 4, 10, 4, -10);
        break;
    case WS_APP_GAMES:
        for (int i = -1; i <= 1; i++) {
            iline(&c, -38, i * 24, 38, i * 24);
            iline(&c, i * 24, -38, i * 24, 38);
        }
        watch_gfx_fill_rect(cx - size * 32 / 100, cy - size * 8 / 100, size / 10, size / 10, color);
        watch_gfx_fill_rect(cx + size * 4 / 100, cy + size * 16 / 100, size / 10, size / 10, color);
        watch_gfx_fill_rect(cx + size * 24 / 100, cy - size * 28 / 100, size / 10, size / 10, color);
        break;
    case WS_APP_SETTINGS:
        icircle(&c, 30);
        icircle(&c, 12);
        for (int i = 0; i < 8; i++) {
            float a = i * 45.0f * PI_F / 180.0f;
            iline(&c, (int)(sinf(a) * 30), (int)(-cosf(a) * 30),
                  (int)(sinf(a) * 44), (int)(-cosf(a) * 44));
        }
        break;
    default:
        icircle(&c, 36);
        break;
    }
}

void watch_gfx_battery(int x, int y, int percent, bool charging, uint16_t color)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    watch_gfx_rect(x, y, 18, 9, color);
    watch_gfx_fill_rect(x + 18, y + 3, 2, 3, color);
    int fill = (16 * percent) / 100;
    watch_gfx_fill_rect(x + 1, y + 1, fill, 7, percent < 20 ? WS_COLOR_RED : color);
    if (charging) {
        watch_gfx_line(x + 8, y + 2, x + 6, y + 5, WS_COLOR_BLACK);
        watch_gfx_line(x + 6, y + 5, x + 10, y + 5, WS_COLOR_BLACK);
        watch_gfx_line(x + 10, y + 5, x + 8, y + 8, WS_COLOR_BLACK);
    }
}

void watch_gfx_wifi(int x, int y, int rssi, uint16_t color)
{
    int bars = rssi > -55 ? 3 : rssi > -70 ? 2 : rssi > -85 ? 1 : 0;
    for (int i = 0; i < 3; i++) {
        int h = 2 + i * 2;
        if (i < bars) {
            watch_gfx_fill_rect(x + i * 3, y + 8 - h, 2, h, color);
        } else {
            watch_gfx_vline(x + i * 3, y + 8 - h, h, WS_COLOR_DIM);
            watch_gfx_vline(x + i * 3 + 1, y + 8 - h, h, WS_COLOR_DIM);
        }
    }
}

void watch_gfx_checkbox(int x, int y, bool on, uint16_t color)
{
    watch_gfx_rect(x, y, 9, 9, color);
    if (on) {
        watch_gfx_line(x + 2, y + 4, x + 4, y + 6, color);
        watch_gfx_line(x + 4, y + 6, x + 8, y + 1, color);
    }
}
