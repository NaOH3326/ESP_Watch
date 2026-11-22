#include "watch_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#ifndef CONFIG_WATCH_MUSIC_URL
#define CONFIG_WATCH_MUSIC_URL ""
#endif
#ifndef CONFIG_WATCH_MUSIC_TOKEN
#define CONFIG_WATCH_MUSIC_TOKEN ""
#endif

static const char *TAG = "WATCH_CFG";
static bool s_ready;

esp_err_t watch_config_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

static esp_err_t open_ns(nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!s_ready) {
        esp_err_t err = watch_config_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    return nvs_open("watch", mode, out);
}

esp_err_t watch_config_get_str(const char *key, char *out, size_t out_len)
{
    if (!key || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = out_len;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return err;
}

esp_err_t watch_config_set_str(const char *key, const char *value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (value && value[0]) {
        err = nvs_set_str(h, key, value);
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

int watch_config_get_int(const char *key, int default_value)
{
    nvs_handle_t h;
    if (open_ns(NVS_READONLY, &h) != ESP_OK) {
        return default_value;
    }
    int32_t v = default_value;
    if (nvs_get_i32(h, key, &v) != ESP_OK) {
        v = default_value;
    }
    nvs_close(h);
    return (int)v;
}

esp_err_t watch_config_set_int(const char *key, int value)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

bool watch_config_has_wifi(void)
{
    char ssid[33] = {0};
    watch_config_get_str("wifi_ssid", ssid, sizeof(ssid));
    if (ssid[0]) {
        return true;
    }
    return strlen(CONFIG_WATCH_WIFI_SSID) > 0;
}

bool watch_config_has_ai(void)
{
    char url[192] = {0};
    watch_config_get_str("ai_url", url, sizeof(url));
    if (url[0]) {
        return true;
    }
    return strlen(CONFIG_WATCH_AI_URL) > 0;
}

bool watch_config_has_video(void)
{
    char url[192] = {0};
    watch_config_get_str("video_url", url, sizeof(url));
    if (url[0]) {
        return true;
    }
    return strlen(CONFIG_WATCH_VIDEO_URL) > 0;
}

bool watch_config_has_music(void)
{
    char url[192] = {0};
    watch_config_get_str("music_url", url, sizeof(url));
    if (url[0]) {
        return true;
    }
    return strlen(CONFIG_WATCH_MUSIC_URL) > 0;
}

void watch_config_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (ssid && ssid_len) {
        watch_config_get_str("wifi_ssid", ssid, ssid_len);
        if (!ssid[0]) {
            snprintf(ssid, ssid_len, "%s", CONFIG_WATCH_WIFI_SSID);
        }
    }
    if (pass && pass_len) {
        watch_config_get_str("wifi_pass", pass, pass_len);
        if (!pass[0]) {
            snprintf(pass, pass_len, "%s", CONFIG_WATCH_WIFI_PASSWORD);
        }
    }
}

void watch_config_get_ai(char *url, size_t url_len, char *token, size_t token_len)
{
    if (url && url_len) {
        watch_config_get_str("ai_url", url, url_len);
        if (!url[0]) {
            snprintf(url, url_len, "%s", CONFIG_WATCH_AI_URL);
        }
    }
    if (token && token_len) {
        watch_config_get_str("ai_token", token, token_len);
        if (!token[0]) {
            snprintf(token, token_len, "%s", CONFIG_WATCH_AI_TOKEN);
        }
    }
}

void watch_config_get_video(char *url, size_t url_len, char *token, size_t token_len)
{
    if (url && url_len) {
        watch_config_get_str("video_url", url, url_len);
        if (!url[0]) {
            snprintf(url, url_len, "%s", CONFIG_WATCH_VIDEO_URL);
        }
    }
    if (token && token_len) {
        watch_config_get_str("video_token", token, token_len);
        if (!token[0]) {
            snprintf(token, token_len, "%s", CONFIG_WATCH_VIDEO_TOKEN);
        }
    }
}

void watch_config_get_music(char *url, size_t url_len, char *token, size_t token_len)
{
    if (url && url_len) {
        watch_config_get_str("music_url", url, url_len);
        if (!url[0]) {
            snprintf(url, url_len, "%s", CONFIG_WATCH_MUSIC_URL);
        }
    }
    if (token && token_len) {
        watch_config_get_str("music_token", token, token_len);
        if (!token[0]) {
            snprintf(token, token_len, "%s", CONFIG_WATCH_MUSIC_TOKEN);
        }
    }
}

void watch_config_set_wifi(const char *ssid, const char *pass)
{
    watch_config_set_str("wifi_ssid", ssid ? ssid : "");
    watch_config_set_str("wifi_pass", pass ? pass : "");
}

void watch_config_set_ai(const char *url, const char *token)
{
    watch_config_set_str("ai_url", url ? url : "");
    watch_config_set_str("ai_token", token ? token : "");
}

void watch_config_set_video(const char *url, const char *token)
{
    watch_config_set_str("video_url", url ? url : "");
    watch_config_set_str("video_token", token ? token : "");
}

void watch_config_set_music(const char *url, const char *token)
{
    watch_config_set_str("music_url", url ? url : "");
    watch_config_set_str("music_token", token ? token : "");
}

void watch_config_clear_wifi(void)
{
    watch_config_set_str("wifi_ssid", "");
    watch_config_set_str("wifi_pass", "");
}
