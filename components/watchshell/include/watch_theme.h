#pragma once

#include <stdbool.h>

typedef enum {
    WS_THEME_KALI = 0,
    WS_THEME_CHAOS,
    WS_THEME_ERROR,
} ws_theme_t;

void watch_theme_init(void);
ws_theme_t watch_theme_current(void);
const char *watch_theme_name(void);
void watch_theme_next(void);
bool watch_theme_unlock_error(void);
bool watch_theme_error_unlocked(void);
void watch_theme_disable_error(void);
void watch_theme_set(ws_theme_t theme);

/* Transform the RGB565 framebuffer in place before it is flushed. */
void watch_theme_apply(void);

/* ERROR theme glitch window: tick once per rendered frame, then draw the
 * overlay after watch_theme_apply() so the popup itself stays readable. */
void watch_theme_tick(void);
void watch_theme_draw_overlay(void);
