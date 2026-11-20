#pragma once

#include "watch_types.h"

typedef struct {
    const char *id;
    const char *name;
    const char *subtitle;
    void (*enter)(void);
    void (*leave)(void);
    void (*render)(void);
    void (*event)(const ws_event_t *ev);
    void (*tick)(void);
} ws_app_impl_t;

extern const ws_app_impl_t ws_app_watchface;
extern const ws_app_impl_t ws_app_ai;
extern const ws_app_impl_t ws_app_shell;
extern const ws_app_impl_t ws_app_sensors;
extern const ws_app_impl_t ws_app_media;
extern const ws_app_impl_t ws_app_games;
extern const ws_app_impl_t ws_app_settings;

#define WS_UI_TOP 22

void ws_app_wrap_text(int x, int y, int max_chars, const char *text, uint16_t color, int max_lines);
