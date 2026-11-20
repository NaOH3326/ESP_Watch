#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WS_STORAGE_ROOT "/storage"
#define WS_SCRIPT_DIR   WS_STORAGE_ROOT "/scripts"
#define WS_MUSIC_DIR    WS_STORAGE_ROOT "/music"
#define WS_VIDEO_DIR    WS_STORAGE_ROOT "/video"

esp_err_t watch_storage_init(void);
bool watch_storage_ready(void);

int watch_storage_list(const char *dir, const char *ext,
                       char names[][48], int max_names);
int watch_storage_list_scripts(char names[][48], int max_names);
int watch_storage_list_music(char names[][48], int max_names);

esp_err_t watch_storage_read_all(const char *path, char **out, size_t *out_len);
/* Read a binary file with an explicit size limit.  prefer_psram is used for
 * large JPEGs so the HTTP/UI internal heap is not exhausted. */
esp_err_t watch_storage_read_all_caps(const char *path, char **out, size_t *out_len,
                                      size_t max_len, bool prefer_psram);
esp_err_t watch_storage_write_all(const char *path, const void *data, size_t len);
esp_err_t watch_storage_delete(const char *path);

const char *watch_storage_error(void);
