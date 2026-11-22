#pragma once

#include <stdbool.h>

#define WS_MUSIC_MAX_ITEMS 24

typedef struct {
    char id[32];
    char title[64];
    char url[192];
    char format[8];
    int duration_ms;
    int size;
} ws_music_item_t;

void watch_media_init(void);

/* Local playback from /storage/music. */
bool watch_media_play(const char *filename);

/* Cloud catalog + streamed playback (HTTP, raw MP3/WAV/AAC bytes). */
void watch_media_catalog_fetch(void);
int watch_media_catalog_count(void);
bool watch_media_catalog_get(int index, ws_music_item_t *out);
bool watch_media_play_remote(int index);

void watch_media_stop(void);
bool watch_media_busy(void);
const char *watch_media_current(void);
