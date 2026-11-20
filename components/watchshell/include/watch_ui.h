#pragma once

#include <stdbool.h>

#include "watch_types.h"

void watch_ui_start(void);
void watch_ui_open_app(ws_app_id_t app);
bool watch_ui_open_app_by_name(const char *name);
const char *watch_ui_app_name(ws_app_id_t app);
void watch_ui_back(void);
void watch_ui_pause_render(void);
void watch_ui_resume_render(void);

/* Inject a synthetic tap (test/diagnostic).  Used by the serial console so a
 * headless test can enter the AI chat page and take a screenshot. */
bool watch_ui_tap(int x, int y);

/* Media apps use this to temporarily hide the status bar and occupy the
 * complete 240x284 panel.  PWR short press or a tap exits fullscreen. */
void watch_ui_set_fullscreen(bool on);
bool watch_ui_fullscreen(void);

/* Games can hide the status bar without transferring touch/PWR ownership to
 * the UI layer.  The app remains responsible for its own exit gestures. */
void watch_ui_set_chrome_hidden(bool on);
bool watch_ui_chrome_hidden(void);
