#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WS_SCREEN_W 240
#define WS_SCREEN_H 284

/* Rounded panel corners: keep edge widgets inside this inset. */
#define WS_SAFE 10
#define WS_TERM_COLS 36
#define WS_TERM_LINES 64

#define WS_COLOR_BLACK   0x0000
#define WS_COLOR_WHITE   0xFFFF
#define WS_COLOR_GREEN   0x3FE6
#define WS_COLOR_DIM     0x0A41
#define WS_COLOR_CYAN    0x07FF
#define WS_COLOR_AMBER   0xFD20
#define WS_COLOR_RED     0xF800
#define WS_COLOR_BLUE    0x001F
#define WS_COLOR_MAGENTA 0xF81F
#define WS_COLOR_GRAY    0x8410

typedef enum {
    WS_EV_NONE = 0,
    WS_EV_TOUCH_DOWN,
    WS_EV_TOUCH_MOVE,
    WS_EV_TOUCH_UP,
    WS_EV_KEY_DOWN,
    WS_EV_KEY_UP,
} ws_event_type_t;

typedef enum {
    WS_KEY_PWR = 0,
    WS_KEY_LEFT,
    WS_KEY_RIGHT,
    WS_KEY_COUNT,
} ws_key_t;

typedef struct {
    ws_event_type_t type;
    int16_t x;
    int16_t y;
    int16_t dx;
    int16_t dy;
    uint8_t key;
} ws_event_t;

typedef enum {
    WS_APP_WATCHFACE = 0,
    WS_APP_AI,
    WS_APP_SHELL,
    WS_APP_SENSORS,
    WS_APP_MEDIA,
    WS_APP_GAMES,
    WS_APP_SETTINGS,
    WS_APP_COUNT,
} ws_app_id_t;
