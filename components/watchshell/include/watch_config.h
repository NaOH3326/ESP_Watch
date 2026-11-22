#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t watch_config_init(void);

/* String values are copied out; empty string means "not configured". */
esp_err_t watch_config_get_str(const char *key, char *out, size_t out_len);
esp_err_t watch_config_set_str(const char *key, const char *value);

int watch_config_get_int(const char *key, int default_value);
esp_err_t watch_config_set_int(const char *key, int value);

bool watch_config_has_wifi(void);
bool watch_config_has_ai(void);
bool watch_config_has_video(void);
bool watch_config_has_music(void);

/* Convenience wrappers. */
void watch_config_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
void watch_config_get_ai(char *url, size_t url_len, char *token, size_t token_len);
void watch_config_get_video(char *url, size_t url_len, char *token, size_t token_len);
void watch_config_get_music(char *url, size_t url_len, char *token, size_t token_len);
void watch_config_set_wifi(const char *ssid, const char *pass);
void watch_config_set_ai(const char *url, const char *token);
void watch_config_set_video(const char *url, const char *token);
void watch_config_set_music(const char *url, const char *token);
void watch_config_clear_wifi(void);
