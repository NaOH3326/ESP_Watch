#include "watch_theme.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "ft_lcd.h"
#include "watch_config.h"
#include "watch_gfx.h"
#include "watch_types.h"

static const char *TAG = "WATCH_THEME";
static ws_theme_t s_theme = WS_THEME_KALI;
static bool s_error_unlocked;

/* Pixels below this luminance are the dot-matrix/scanline background.
 * KALI/CHAOS keep that field green; ERROR turns its green dots red. */
#define THEME_BG_LUMA 28

static bool s_ov_active;
static int64_t s_ov_until;
static int64_t s_ov_next;
static int s_ov_x, s_ov_y, s_ov_w, s_ov_h;
static uint32_t s_ov_code;

static const char *s_names[] = {"KALI", "CHAOS", "ERROR"};

static void save(void)
{
    watch_config_set_int("theme", (int)s_theme);
    watch_config_set_int("err_unlock", s_error_unlocked ? 1 : 0);
}

void watch_theme_init(void)
{
    int theme = watch_config_get_int("theme", WS_THEME_KALI);
    s_error_unlocked = watch_config_get_int("err_unlock", 0) != 0;
    if (theme < WS_THEME_KALI || theme > WS_THEME_ERROR) {
        theme = WS_THEME_KALI;
    }
    if (theme == WS_THEME_ERROR && !s_error_unlocked) {
        theme = WS_THEME_KALI;
    }
    s_theme = (ws_theme_t)theme;
    s_ov_active = false;
    s_ov_next = 0;
    s_ov_until = 0;
    save();
    ESP_LOGI(TAG, "theme=%s unlocked=%d", s_names[s_theme], s_error_unlocked);
}

ws_theme_t watch_theme_current(void) { return s_theme; }

const char *watch_theme_name(void) { return s_names[s_theme]; }

bool watch_theme_error_unlocked(void) { return s_error_unlocked; }

void watch_theme_next(void)
{
    if (s_theme == WS_THEME_ERROR) {
        /* Turning ERROR off in Settings consumes the easter egg.  It must be
         * unlocked again from the watchface. */
        watch_theme_disable_error();
    } else {
        s_theme = (s_theme == WS_THEME_KALI) ? WS_THEME_CHAOS : WS_THEME_KALI;
    }
    save();
    ESP_LOGI(TAG, "theme=%s unlocked=%d", s_names[s_theme], s_error_unlocked);
}

bool watch_theme_unlock_error(void)
{
    bool changed = !s_error_unlocked || s_theme != WS_THEME_ERROR;
    s_error_unlocked = true;
    s_theme = WS_THEME_ERROR;
    s_ov_active = false;
    s_ov_next = 0;
    s_ov_until = 0;
    save();
    if (changed) {
        ESP_LOGW(TAG, "easter egg: ERROR theme unlocked");
    }
    return changed;
}

void watch_theme_disable_error(void)
{
    s_theme = WS_THEME_KALI;
    s_error_unlocked = false;
    s_ov_active = false;
    s_ov_next = 0;
    s_ov_until = 0;
    save();
    ESP_LOGI(TAG, "ERROR theme locked again");
}

void watch_theme_set(ws_theme_t theme)
{
    if (theme < WS_THEME_KALI || theme > WS_THEME_ERROR) return;
    s_theme = theme;
    if (theme == WS_THEME_ERROR) {
        s_error_unlocked = true;
    }
    s_ov_active = false;
    s_ov_next = 0;
    s_ov_until = 0;
    save();
}

static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void watch_theme_apply(void)
{
    if (s_theme == WS_THEME_KALI) return;
    const ft_lcd_info_t *info = ft_lcd_info();
    uint16_t *fb = (uint16_t *)ft_lcd_framebuffer();
    if (!info || !fb) return;

    int64_t ms = esp_timer_get_time() / 1000;
    uint32_t t = (uint32_t)ms;
    int w = info->width;
    int h = info->height;

    for (int y = 0; y < h; y++) {
        uint16_t *row = fb + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint16_t rgb = swap16(row[x]);
            if (rgb == 0) continue;
            int r = (rgb >> 11) & 0x1F;
            int g = (rgb >> 5) & 0x3F;
            int b = rgb & 0x1F;
            int r8 = (r << 3) | (r >> 2);
            int g8 = (g << 2) | (g >> 4);
            int b8 = (b << 3) | (b >> 2);
            int lum = (r8 * 3 + g8 * 6 + b8) / 10;

            /* Keep dark backgrounds untouched, except ERROR turns the green
             * dot-matrix/scanline field into an equally dim red field. */
            if (lum < THEME_BG_LUMA) {
                if (s_theme == WS_THEME_ERROR && g8 > r8 + 4 && g8 > b8 + 4) {
                    int rr = clamp8(g8 * 2);
                    uint16_t red = (uint16_t)(((rr & 0xF8) << 8));
                    row[x] = swap16(red);
                }
                continue;
            }

            if (s_theme == WS_THEME_CHAOS) {
                int nr = (r8 + x * 3 + (int)(t / 8)) & 0xFF;
                int ng = (g8 + y * 3 + (int)(t / 5)) & 0xFF;
                int nb = (b8 + x * 2 + y * 2 + (int)(t / 3)) & 0xFF;
                r8 = nr; g8 = ng; b8 = nb;
            } else { /* ERROR */
                int boost = (y % 19 == 0) ? 36 : 0;
                r8 = clamp8(lum * 2 + 40 + boost);
                g8 = 0;
                b8 = 0;
            }
            rgb = (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
            row[x] = swap16(rgb);
        }
    }

    /* ERROR: torn scanline bands.  A few bands jump sideways and one band
     * copies content from another row, giving the "broken framebuffer" look
     * instead of a flat red filter. */
    if (s_theme == WS_THEME_ERROR) {
        uint32_t r = (uint32_t)(t / 90) * 747796405u + 2891336453u;
        int bands = 1 + (int)(r % 3);
        for (int b = 0; b < bands; b++) {
            r = r * 1664525u + 1013904223u;
            int bh = 1 + (int)((r >> 8) % 9);
            if (bh > h) bh = h;
            int y0 = (int)((r >> 16) % (uint32_t)(h - bh + 1));
            int sh = 3 + (int)((r >> 20) % 16);
            if ((r >> 28) & 1) sh = -sh;

            if (b == 0) {
                int src = (y0 + h / 2) % (h - bh + 1);
                memmove(fb + (size_t)y0 * w, fb + (size_t)src * w,
                        (size_t)bh * w * sizeof(uint16_t));
            }
            for (int yy = y0; yy < y0 + bh; yy++) {
                uint16_t *row = fb + (size_t)yy * w;
                if (sh > 0 && sh < w) {
                    memmove(row + sh, row, (size_t)(w - sh) * sizeof(uint16_t));
                    memset(row, 0, (size_t)sh * sizeof(uint16_t));
                } else if (sh < 0 && -sh < w) {
                    int n = -sh;
                    memmove(row, row + n, (size_t)(w - n) * sizeof(uint16_t));
                    memset(row + w - n, 0, (size_t)n * sizeof(uint16_t));
                }
            }
        }
    }
}

void watch_theme_tick(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (s_theme != WS_THEME_ERROR) {
        s_ov_active = false;
        s_ov_next = 0;
        return;
    }
    if (s_ov_active) {
        if (now >= s_ov_until) {
            s_ov_active = false;
            s_ov_next = now + 1600 + (int64_t)(esp_random() % 4000);
        }
        return;
    }
    if (s_ov_next == 0) {
        s_ov_next = now + 800 + (int64_t)(esp_random() % 1800);
        return;
    }
    if (now < s_ov_next) return;

    s_ov_w = 154;
    s_ov_h = 46;
    s_ov_x = 14 + (int)(esp_random() % (WS_SCREEN_W - s_ov_w - 28));
    s_ov_y = 42 + (int)(esp_random() % 116);
    s_ov_code = 0x100 + esp_random() % 0xEFF;
    s_ov_active = true;
    s_ov_until = now + 1000 + (int64_t)(esp_random() % 1800);
}

void watch_theme_draw_overlay(void)
{
    if (s_theme != WS_THEME_ERROR || !s_ov_active) return;

    int x = s_ov_x, y = s_ov_y;
    int w = s_ov_w, h = s_ov_h;
    watch_gfx_fill_rect(x, y, w, h, WS_COLOR_BLACK);
    watch_gfx_fill_rect(x, y, w, 16, WS_COLOR_RED);
    watch_gfx_text(x + 4, y + 3, "SYSTEM ERROR", WS_COLOR_WHITE, 1);
    watch_gfx_rect(x, y, w, h, WS_COLOR_RED);
    watch_gfx_printf(x + 6, y + 22, WS_COLOR_RED, 1, "FAULT 0x%04X", s_ov_code);
    watch_gfx_text(x + 6, y + 34, "MEM 0x0000 / CORE 1", WS_COLOR_AMBER, 1);
}

