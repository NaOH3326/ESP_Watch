#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WS_VIDEO_OFFLINE = 0,
    WS_VIDEO_FETCHING,
    WS_VIDEO_READY,
    WS_VIDEO_CONNECTING,
    WS_VIDEO_PLAYING,
    WS_VIDEO_ERROR,
} ws_video_state_t;

typedef struct {
    char id[32];
    char title[48];
    char stream_url[192];
    int duration_ms;
    int width;
    int height;
} ws_video_item_t;

typedef struct {
    bool running;       /* video tab is enabled */
    bool connected;     /* a sender/stream is connected */
    bool has_frame;     /* a decoded frame is available */
    int width;
    int height;
    unsigned frames;
    unsigned audio_chunks;
    int fps;
    int port;
    char ip[16];
    ws_video_state_t state;
    int catalog_count;
    char title[48];
    char detail[64];
} ws_video_status_t;

/* The video module keeps the legacy WSV2 TCP receiver for local senders and
 * also supports a catalog + HTTP pull mode.  The watch fetches the catalog,
 * then opens one item's stream_url and reads WSV2 frames from the HTTP body. */
void watch_video_start(void);
void watch_video_stop(void);
void watch_video_get_status(ws_video_status_t *out);

void watch_video_catalog_fetch(void);
int watch_video_catalog_count(void);
bool watch_video_catalog_get(int index, ws_video_item_t *out);
void watch_video_play(int index);
void watch_video_stop_playback(void);
const char *watch_video_state_name(ws_video_state_t state);

/* Draw the latest decoded RGB565 frame, scaled to fit max_w x max_h. */
bool watch_video_blit(int x, int y, int max_w, int max_h);
/* Draw the frame edge-to-edge, cropping the overflow for fullscreen. */
bool watch_video_blit_cover(int x, int y, int max_w, int max_h);
