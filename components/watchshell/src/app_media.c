#include "watch_apps.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "watch_audio.h"
#include "watch_config.h"
#include "watch_gfx.h"
#include "watch_media.h"
#include "watch_storage.h"
#include "watch_ui.h"
#include "watch_video.h"

#define IMG_READ_MAX        (6 * 1024 * 1024)
#define IMG_DECODE_MAX      (1 * 1024 * 1024)
#define MEDIA_TAB_W         72
#define MEDIA_TAB_STEP      74
#define MEDIA_DEL_X         WS_SAFE
#define MEDIA_DEL_Y         250
#define MEDIA_DEL_W         80
#define MEDIA_DEL_H         22
#define MEDIA_VIDEO_BTN_Y   250
#define MEDIA_VIDEO_BTN_W   68
#define MEDIA_VIDEO_BTN_H   22

static char s_files[24][48];
static int s_count;
static int s_sel;
static int s_top;
static bool s_was_busy;
static int s_tab;
static uint16_t *s_img;
static int s_img_w;
static int s_img_h;
static char s_img_name[48];
static char s_img_msg[48];
static char s_selected_name[48];
static bool s_delete_armed;
static char s_delete_name[48];
static int64_t s_delete_until;
static int64_t s_last_refresh;
static bool s_video_configured;
static bool s_music_configured;
static bool s_music_remote;

static void free_img(void)
{
    if (s_img) {
        free(s_img);
        s_img = NULL;
    }
    s_img_w = s_img_h = 0;
    s_img_name[0] = '\0';
}

static void scroll_to_sel(void)
{
    int rows = s_tab == 0 ? 10 : (s_tab == 1 ? 3 : 3);
    if (rows <= 0) {
        s_top = 0;
        return;
    }
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + rows) s_top = s_sel - rows + 1;
    int max_top = s_count > rows ? s_count - rows : 0;
    if (s_top > max_top) s_top = max_top;
    if (s_top < 0) s_top = 0;
}

static void refresh(void)
{
    char keep[48];
    snprintf(keep, sizeof(keep), "%s", s_selected_name);

    if (s_tab == 2) {
        s_count = watch_video_catalog_count();
        if (s_sel >= s_count) s_sel = s_count ? s_count - 1 : 0;
        scroll_to_sel();
        return;
    }

    if (s_tab == 0) {
        /* Cloud catalog takes priority; fall back to /storage/music when it
         * is not configured or has no playable entries. */
        s_count = s_music_configured ? watch_media_catalog_count() : 0;
        s_music_remote = s_count > 0;
        if (!s_count) {
            s_count = watch_storage_list_music(s_files, 24);
        }
    } else if (s_tab == 1) {
        s_count = watch_storage_list(WS_VIDEO_DIR, ".jpg,.jpeg", s_files, 24);
    } else {
        s_count = 0;
    }

    if (s_tab == 1 && keep[0]) {
        for (int i = 0; i < s_count; i++) {
            if (!strcmp(s_files[i], keep)) {
                s_sel = i;
                break;
            }
        }
    }
    if (s_sel >= s_count) s_sel = s_count ? s_count - 1 : 0;
    if (s_tab == 1 && s_count) {
        snprintf(s_selected_name, sizeof(s_selected_name), "%s", s_files[s_sel]);
        if (s_img_name[0] && strcmp(s_img_name, s_selected_name) != 0) {
            free_img();
        }
    } else {
        s_selected_name[0] = '\0';
    }
    scroll_to_sel();
}

static void load_image(void)
{
    if (s_tab != 1 || !s_count) return;

    free_img();
    s_img_msg[0] = '\0';

    char path[128];
    snprintf(path, sizeof(path), "%s/%s", WS_VIDEO_DIR, s_files[s_sel]);
    char *data = NULL;
    size_t len = 0;
    if (watch_storage_read_all_caps(path, &data, &len, IMG_READ_MAX, true) != ESP_OK) {
        snprintf(s_img_msg, sizeof(s_img_msg), "READ FAIL");
        return;
    }

    static const esp_jpeg_image_scale_t scales[] = {
        JPEG_IMAGE_SCALE_0,
        JPEG_IMAGE_SCALE_1_2,
        JPEG_IMAGE_SCALE_1_4,
        JPEG_IMAGE_SCALE_1_8,
    };
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)data,
        .indata_size = (uint32_t)len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
    };

    for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
        cfg.out_scale = scales[i];
        cfg.outbuf = NULL;
        cfg.outbuf_size = 0;
        esp_jpeg_image_output_t info = {0};
        if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || !info.output_len) {
            break;
        }
        if (info.output_len > IMG_DECODE_MAX) {
            continue;   /* try the next lower resolution */
        }

        uint16_t *buf = heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = malloc(info.output_len);
        if (!buf) {
            continue;
        }

        cfg.outbuf = (uint8_t *)buf;
        cfg.outbuf_size = (uint32_t)info.output_len;
        if (esp_jpeg_decode(&cfg, &info) == ESP_OK) {
            s_img = buf;
            s_img_w = info.width;
            s_img_h = info.height;
            snprintf(s_img_name, sizeof(s_img_name), "%s", s_files[s_sel]);
            free(data);
            return;
        }
        free(buf);
        cfg.outbuf = NULL;
        cfg.outbuf_size = 0;
    }

    free(data);
    snprintf(s_img_msg, sizeof(s_img_msg), "DECODE FAIL");
}

static void delete_picture(const char *name)
{
    if (s_tab != 1 || !name || !name[0]) return;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", WS_VIDEO_DIR, name);
    if (watch_storage_delete(path) != ESP_OK) {
        snprintf(s_img_msg, sizeof(s_img_msg), "DELETE FAIL");
        return;
    }

    free_img();
    refresh();
    if (s_count) {
        load_image();
    } else {
        snprintf(s_img_msg, sizeof(s_img_msg), "NO PICTURES");
    }
}

static void media_enter(void)
{
    s_tab = 0;
    s_sel = 0;
    s_top = 0;
    free_img();
    s_img_msg[0] = '\0';
    s_selected_name[0] = '\0';
    s_delete_armed = false;
    s_delete_name[0] = '\0';
    s_last_refresh = esp_timer_get_time();
    watch_ui_set_fullscreen(false);
    watch_media_init();
    s_video_configured = watch_config_has_video();
    s_music_configured = watch_config_has_music();
    if (s_music_configured) watch_media_catalog_fetch();
    refresh();
}

static void media_leave(void)
{
    watch_ui_set_fullscreen(false);
    watch_media_stop();
    watch_video_stop();
    free_img();
}

static void play_selected(void)
{
    if (!s_count || s_tab != 0) return;
    watch_media_stop();
    if (s_music_remote) {
        watch_media_play_remote(s_sel);
    } else {
        watch_media_play(s_files[s_sel]);
    }
}

static void switch_tab(int tab)
{
    if (tab == s_tab) return;

    watch_ui_set_fullscreen(false);
    if (s_tab == 2) watch_video_stop();
    if (tab == 2) watch_media_stop();

    s_tab = tab;
    s_sel = 0;
    s_top = 0;
    free_img();
    s_img_msg[0] = '\0';
    s_selected_name[0] = '\0';
    s_delete_armed = false;
    s_delete_name[0] = '\0';
    s_last_refresh = esp_timer_get_time();
    refresh();
    if (s_tab == 1 && s_count) load_image();
    if (s_tab == 0) {
        s_music_configured = watch_config_has_music();
        if (s_music_configured) watch_media_catalog_fetch();
        refresh();
    }
    if (s_tab == 2) {
        s_video_configured = watch_config_has_video();
        watch_video_start();
        watch_video_catalog_fetch();
    }
}

static void media_event(const ws_event_t *ev)
{
    if (ev->type == WS_EV_KEY_DOWN) {
        bool changed = false;
        if (ev->key == WS_KEY_LEFT && s_sel > 0) {
            s_sel--;
            changed = true;
        } else if (ev->key == WS_KEY_RIGHT && s_sel + 1 < s_count) {
            s_sel++;
            changed = true;
        }
        if (changed) {
            s_delete_armed = false;
            s_delete_name[0] = '\0';
            if (s_tab == 1) {
                snprintf(s_selected_name, sizeof(s_selected_name), "%s", s_files[s_sel]);
            }
            scroll_to_sel();
            if (s_tab == 1) load_image();
        }
        return;
    }
    if (ev->type != WS_EV_TOUCH_UP) return;
    if (ev->y >= 24 && ev->y <= 38) {
        int tab = ev->x < WS_SAFE + MEDIA_TAB_STEP ? 0 :
                  (ev->x < WS_SAFE + 2 * MEDIA_TAB_STEP ? 1 : 2);
        switch_tab(tab);
        return;
    }
    if (s_tab == 2 && ev->y >= MEDIA_VIDEO_BTN_Y) {
        int step = MEDIA_VIDEO_BTN_W + 4;
        if (ev->x < WS_SAFE + step) {
            watch_video_catalog_fetch();
        } else if (ev->x < WS_SAFE + 2 * step) {
            watch_video_play(s_sel);
        } else {
            watch_video_stop_playback();
        }
        return;
    }
    if (s_tab == 1 && s_img && ev->y >= 92 && ev->y <= 244) {
        watch_ui_set_fullscreen(true);
        return;
    }
    if (s_tab == 2 && ev->y >= 92 && ev->y <= 244) {
        ws_video_status_t vs = {0};
        watch_video_get_status(&vs);
        if (vs.has_frame) {
            watch_ui_set_fullscreen(true);
        }
        return;
    }
    if (s_tab == 0 && ev->y > 246) {
        if (ev->x < 82) {
            watch_media_stop();
        } else if (ev->x < 158) {
            int v = watch_audio_volume() - 10;
            if (v < 0) v = 0;
            watch_audio_set_volume(v);
            watch_config_set_int("volume", v);
        } else {
            int v = watch_audio_volume() + 10;
            if (v > 100) v = 100;
            watch_audio_set_volume(v);
            watch_config_set_int("volume", v);
        }
        return;
    }
    if (s_tab == 1 && ev->x >= MEDIA_DEL_X && ev->x < MEDIA_DEL_X + MEDIA_DEL_W &&
        ev->y >= MEDIA_DEL_Y && ev->y < MEDIA_DEL_Y + MEDIA_DEL_H) {
        if (!s_count) {
            snprintf(s_img_msg, sizeof(s_img_msg), "NO PICTURES");
        } else if (!s_delete_armed) {
            s_delete_armed = true;
            snprintf(s_delete_name, sizeof(s_delete_name), "%s",
                     s_selected_name[0] ? s_selected_name : s_files[s_sel]);
            s_delete_until = esp_timer_get_time() + 3000000;
        } else {
            s_delete_armed = false;
            char name[48];
            snprintf(name, sizeof(name), "%s", s_delete_name);
            s_delete_name[0] = '\0';
            delete_picture(name);
        }
        return;
    }
    int rows = s_tab == 0 ? 10 : (s_tab == 1 ? 3 : 3);
    if (rows > 0 && ev->y >= 54 && ev->y < 54 + rows * 10) {
        int row = (ev->y - 54) / 10;
        int idx = s_top + row;
        if (idx >= 0 && idx < s_count) {
            s_sel = idx;
            s_delete_armed = false;
            s_delete_name[0] = '\0';
            if (s_tab == 0) {
                play_selected();
            } else if (s_tab == 2) {
                scroll_to_sel();
                watch_video_play(s_sel);
            } else {
                snprintf(s_selected_name, sizeof(s_selected_name), "%s", s_files[s_sel]);
                load_image();
            }
        }
    }
}

static void media_fullscreen_render(void)
{
    watch_gfx_clear(WS_COLOR_BLACK);
    if (s_tab == 1 && s_img && s_img_w > 0 && s_img_h > 0) {
        int max_w = WS_SCREEN_W, max_h = WS_SCREEN_H;
        int w = max_w, h = max_h;
        if ((int64_t)s_img_w * max_h > (int64_t)s_img_h * max_w) {
            w = s_img_w * max_h / s_img_h;
        } else {
            h = s_img_h * max_w / s_img_w;
        }
        int x = (max_w - w) / 2;
        int y = (max_h - h) / 2;
        watch_gfx_blit_scaled(x, y, w, h, s_img, s_img_w, s_img_h);
        return;
    }

    if (s_tab == 2) {
        if (!watch_video_blit_cover(0, 0, WS_SCREEN_W, WS_SCREEN_H)) {
            watch_gfx_text_center(120, 130, "WAITING FOR VIDEO", WS_COLOR_AMBER, 1);
            watch_gfx_text_center(120, 148, "TAP OR PWR TO EXIT", WS_COLOR_DIM, 1);
        }
        return;
    }

    watch_gfx_text_center(120, 130, "NO MEDIA", WS_COLOR_AMBER, 1);
}

static void media_render(void)
{
    if (watch_ui_fullscreen()) {
        media_fullscreen_render();
        return;
    }

    static const char *const tab_names[] = { "MUSIC", "PICTURES", "VIDEO" };
    ws_video_status_t vs = {0};
    if (s_tab == 2) watch_video_get_status(&vs);

    for (int i = 0; i < 3; i++) {
        int x = WS_SAFE + i * MEDIA_TAB_STEP;
        bool sel = i == s_tab;
        watch_gfx_rect(x, 24, MEDIA_TAB_W, 14, sel ? WS_COLOR_GREEN : WS_COLOR_DIM);
        if (sel) watch_gfx_fill_rect(x + 1, 25, MEDIA_TAB_W - 2, 12, 0x02A0);
        watch_gfx_text_center(x + MEDIA_TAB_W / 2, 27, tab_names[i],
                              sel ? WS_COLOR_BLACK : WS_COLOR_GRAY, 1);
    }
    const char *right = s_tab == 0 ? (watch_media_busy() ? "PLAYING" : "STOP") :
                        (s_tab == 1 ? "JPG TAP=FULL" :
                         watch_video_state_name(vs.state));
    uint16_t right_color = s_tab == 0 && watch_media_busy() ? WS_COLOR_GREEN :
                           (s_tab == 2 && vs.connected ? WS_COLOR_GREEN :
                            (s_tab == 2 && vs.state == WS_VIDEO_ERROR ? WS_COLOR_RED :
                             WS_COLOR_DIM));
    watch_gfx_text_right(WS_SCREEN_W - WS_SAFE, 42, right, right_color, 1);
    watch_gfx_hline(WS_SAFE, 52, WS_SCREEN_W - 2 * WS_SAFE, WS_COLOR_DIM);

    if (s_tab != 2 && !s_count) {
        const char *empty = s_tab == 0 ?
            (s_music_configured ? "NO CLOUD MUSIC" : "NO MUSIC FILES") : "NO PICTURES";
        watch_gfx_text(WS_SAFE, 66, empty, WS_COLOR_AMBER, 1);
        watch_gfx_text(WS_SAFE, 82, s_tab == 0 && s_music_configured ?
                       "Check MUSIC CATALOG" : "Upload via Settings /",
                       WS_COLOR_GRAY, 1);
        watch_gfx_text(WS_SAFE, 94, s_tab == 0 && s_music_configured ?
                       "or upload to server." : "portal page.", WS_COLOR_GRAY, 1);
    }
    int rows = s_tab == 0 ? 10 : (s_tab == 1 ? 3 : 3);
    for (int row = 0; row < rows; row++) {
        int idx = s_top + row;
        if (idx >= s_count) break;
        int y = 56 + row * 10;
        bool sel = idx == s_sel;
        if (sel) watch_gfx_fill_rect(8, y - 1, WS_SCREEN_W - 16, 9, 0x02A0);
        if (s_tab == 2) {
            ws_video_item_t it;
            if (!watch_video_catalog_get(idx, &it)) break;
            watch_gfx_printf(WS_SAFE, y, sel ? WS_COLOR_BLACK : WS_COLOR_GRAY, 1,
                             "%c %.31s", sel ? '>' : ' ', it.title);
        } else if (s_tab == 0 && s_music_remote) {
            ws_music_item_t it;
            if (!watch_media_catalog_get(idx, &it)) break;
            watch_gfx_printf(WS_SAFE, y, sel ? WS_COLOR_BLACK : WS_COLOR_GRAY, 1,
                             "%c %.31s", sel ? '>' : ' ', it.title);
        } else {
            watch_gfx_printf(WS_SAFE, y, sel ? WS_COLOR_BLACK : WS_COLOR_GRAY, 1,
                             "%c %s", sel ? '>' : ' ', s_files[idx]);
        }
    }

    if (s_tab == 0) {
        watch_gfx_hline(WS_SAFE, 164, WS_SCREEN_W - 2 * WS_SAFE, WS_COLOR_DIM);
        const char *cur = watch_media_current();
        watch_gfx_text(WS_SAFE, 174, "NOW", WS_COLOR_DIM, 1);
        if (cur && cur[0]) {
            watch_gfx_text(WS_SAFE, 186, cur, WS_COLOR_WHITE, 1);
            int vol = watch_audio_volume();
            watch_gfx_text(WS_SAFE, 202, "VOL", WS_COLOR_DIM, 1);
            watch_gfx_rect(40, 202, 146, 7, WS_COLOR_DIM);
            watch_gfx_fill_rect(41, 203, 144 * vol / 100, 5, WS_COLOR_GREEN);
            watch_gfx_printf(190, 202, WS_COLOR_GREEN, 1, "%3d%%", vol);
            if (watch_media_busy()) {
                int phase = (int)((esp_timer_get_time() / 150000) % 12);
                for (int i = 0; i < 12; i++) {
                    int h = 3 + (i * 7 + phase) % 18;
                    watch_gfx_vline(60 + i * 10, 228 - h, h, i % 3 ? WS_COLOR_GREEN : WS_COLOR_CYAN);
                }
            }
        } else {
            watch_gfx_text(WS_SAFE, 186, "nothing playing", WS_COLOR_GRAY, 1);
        }
        watch_gfx_rect(WS_SAFE, 250, 72, 22, WS_COLOR_RED);
        watch_gfx_text_center(WS_SAFE + 36, 256, "STOP", WS_COLOR_RED, 1);
        watch_gfx_rect(WS_SAFE + 74, 250, 72, 22, WS_COLOR_CYAN);
        watch_gfx_text_center(WS_SAFE + 110, 256, "VOL -", WS_COLOR_CYAN, 1);
        watch_gfx_rect(WS_SAFE + 148, 250, 72, 22, WS_COLOR_CYAN);
        watch_gfx_text_center(WS_SAFE + 184, 256, "VOL +", WS_COLOR_CYAN, 1);
    } else if (s_tab == 1) {
        watch_gfx_hline(WS_SAFE, 90, WS_SCREEN_W - 2 * WS_SAFE, WS_COLOR_DIM);
        if (s_img && s_img_w > 0 && s_img_h > 0) {
            int max_w = WS_SCREEN_W - 2 * WS_SAFE, max_h = 138;
            int w = max_w, h = max_h;
            if ((int64_t)s_img_w * max_h > (int64_t)s_img_h * max_w) {
                h = s_img_h * max_w / s_img_w;
            } else {
                w = s_img_w * max_h / s_img_h;
            }
            int x = (WS_SCREEN_W - w) / 2;
            int y = 94 + (max_h - h) / 2;
            watch_gfx_fill_rect(WS_SAFE, 94, max_w, max_h, WS_COLOR_BLACK);
            watch_gfx_blit_scaled(x, y, w, h, s_img, s_img_w, s_img_h);
            char shown[36];
            snprintf(shown, sizeof(shown), "%.35s", s_img_name);
            watch_gfx_text(WS_SAFE, 238, shown, WS_COLOR_CYAN, 1);
        } else {
            const char *msg = s_img_msg[0] ? s_img_msg : "TAP A JPG";
            watch_gfx_text_center(120, 150, msg,
                                  s_img_msg[0] ? WS_COLOR_RED : WS_COLOR_AMBER, 1);
            watch_gfx_text_center(120, 168, s_img_msg[0] ? "check jpg / size" : "decode to screen",
                                  WS_COLOR_GRAY, 1);
        }

        bool can_delete = s_count > 0;
        uint16_t del_color = s_delete_armed ? WS_COLOR_WHITE :
                             (can_delete ? WS_COLOR_RED : WS_COLOR_DIM);
        watch_gfx_rect(MEDIA_DEL_X, MEDIA_DEL_Y, MEDIA_DEL_W, MEDIA_DEL_H, del_color);
        watch_gfx_text_center(MEDIA_DEL_X + MEDIA_DEL_W / 2, 256,
                              s_delete_armed ? "CONFIRM" : "DELETE", del_color, 1);
        if (s_delete_armed) {
            watch_gfx_text(MEDIA_DEL_X + MEDIA_DEL_W + 10, 256, "TAP AGAIN",
                           WS_COLOR_AMBER, 1);
        }
    } else {
        if (!s_count) {
            uint16_t state_color = vs.state == WS_VIDEO_ERROR ? WS_COLOR_RED :
                                   (vs.state == WS_VIDEO_FETCHING ? WS_COLOR_AMBER :
                                    WS_COLOR_GRAY);
            watch_gfx_text(WS_SAFE, 56, "REMOTE CATALOG", WS_COLOR_CYAN, 1);
            watch_gfx_text(WS_SAFE, 68, vs.detail[0] ? vs.detail : "not loaded", state_color, 1);
            watch_gfx_text(WS_SAFE, 80, s_video_configured ?
                           "TAP SYNC TO FETCH" : "SET VIDEO URL IN PORTAL",
                           s_video_configured ? WS_COLOR_GRAY : WS_COLOR_AMBER, 1);
        }

        watch_gfx_hline(WS_SAFE, 90, WS_SCREEN_W - 2 * WS_SAFE, WS_COLOR_DIM);
        int max_w = WS_SCREEN_W - 2 * WS_SAFE, max_h = 138;
        watch_gfx_fill_rect(WS_SAFE, 94, max_w, max_h, WS_COLOR_BLACK);
        if (!watch_video_blit(WS_SAFE, 94, max_w, max_h)) {
            const char *wait = vs.state == WS_VIDEO_CONNECTING ? "CONNECTING..." :
                               (vs.state == WS_VIDEO_FETCHING ? "LOADING CATALOG..." :
                                (s_count ? "SELECT A VIDEO" : "NO VIDEO FRAMES"));
            watch_gfx_text_center(120, 150, wait, WS_COLOR_AMBER, 1);
            watch_gfx_text_center(120, 168,
                                  s_count ? "TAP A ROW OR PLAY" : "TAP SYNC / START SENDER",
                                  WS_COLOR_GRAY, 1);
        }
        if (vs.has_frame) {
            watch_gfx_printf(WS_SAFE, 238, WS_COLOR_CYAN, 1, "%dx%d  %d FPS",
                             vs.width, vs.height, vs.fps);
            watch_gfx_text_right(WS_SCREEN_W - WS_SAFE, 238,
                                 vs.audio_chunks ? "A/V" : "VIDEO",
                                 vs.audio_chunks ? WS_COLOR_GREEN : WS_COLOR_AMBER, 1);
        } else if (vs.title[0]) {
            watch_gfx_text(WS_SAFE, 238, vs.title, WS_COLOR_CYAN, 1);
        }

        int step = MEDIA_VIDEO_BTN_W + 4;
        int bx = WS_SAFE;
        watch_gfx_rect(bx, MEDIA_VIDEO_BTN_Y, MEDIA_VIDEO_BTN_W, MEDIA_VIDEO_BTN_H, WS_COLOR_CYAN);
        watch_gfx_text_center(bx + MEDIA_VIDEO_BTN_W / 2, 256, "SYNC", WS_COLOR_CYAN, 1);
        bx += step;
        watch_gfx_rect(bx, MEDIA_VIDEO_BTN_Y, MEDIA_VIDEO_BTN_W, MEDIA_VIDEO_BTN_H, WS_COLOR_GREEN);
        watch_gfx_text_center(bx + MEDIA_VIDEO_BTN_W / 2, 256, "PLAY", WS_COLOR_GREEN, 1);
        bx += step;
        watch_gfx_rect(bx, MEDIA_VIDEO_BTN_Y, MEDIA_VIDEO_BTN_W, MEDIA_VIDEO_BTN_H, WS_COLOR_RED);
        watch_gfx_text_center(bx + MEDIA_VIDEO_BTN_W / 2, 256, "STOP", WS_COLOR_RED, 1);
    }
}

static void media_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (s_delete_armed && now >= s_delete_until) {
        s_delete_armed = false;
        s_delete_name[0] = '\0';
    }
    bool busy = watch_media_busy();
    if (s_was_busy && !busy) refresh();
    if (s_tab == 1 && now - s_last_refresh > 1500000) {
        s_last_refresh = now;
        int old_count = s_count;
        refresh();
        if (s_count == 0 && old_count != 0) free_img();
    } else if (s_tab == 0 && s_music_configured && now - s_last_refresh > 500000) {
        s_last_refresh = now;
        refresh();
    } else if (s_tab == 2 && now - s_last_refresh > 500000) {
        s_last_refresh = now;
        int old_count = s_count;
        refresh();
        if (s_count != old_count) s_last_refresh = now;
    }
    s_was_busy = busy;
}

const ws_app_impl_t ws_app_media = {
    .id = "media",
    .name = "MEDIA",
    .subtitle = "music / pictures / video",
    .enter = media_enter,
    .leave = media_leave,
    .render = media_render,
    .event = media_event,
    .tick = media_tick,
};
