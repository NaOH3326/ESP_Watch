#pragma once

#include <stdbool.h>

void watch_media_init(void);
bool watch_media_play(const char *filename);
void watch_media_stop(void);
bool watch_media_busy(void);
const char *watch_media_current(void);
