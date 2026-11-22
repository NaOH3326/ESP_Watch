#include "watch_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "watch_audio.h"
#include "watch_storage.h"

static const char *TAG = "WATCH_MEDIA";
static volatile bool s_busy;
static volatile bool s_stop;
static char s_current[48];
static TaskHandle_t s_task;

static bool suffix(const char *s, const char *e)
{
    size_t n = strlen(s), m = strlen(e);
    return n >= m && !strcasecmp(s + n - m, e);
}

static void media_task(void *arg)
{
    char *path = (char *)arg;
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        goto done;
    }
    esp_audio_simple_dec_type_t type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (suffix(path, ".wav")) type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    else if (suffix(path, ".aac") || suffix(path, ".m4a")) type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = type,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t dec = NULL;
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &dec);
    if (ret != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "decoder open failed: %d", ret);
        fclose(f);
        goto done;
    }
    uint8_t *in = malloc(4096);
    uint8_t *pcm = malloc(16384);
    if (!in || !pcm) {
        free(in); free(pcm);
        esp_audio_simple_dec_close(dec);
        fclose(f);
        goto done;
    }
    while (!s_stop && !feof(f)) {
        size_t got = fread(in, 1, 4096, f);
        if (!got) break;
        esp_audio_simple_dec_raw_t raw = { .buffer = in, .len = (uint32_t)got };
        while (raw.len > 0 && !s_stop) {
            esp_audio_simple_dec_out_t out = { .buffer = pcm, .len = 16384 };
            ret = esp_audio_simple_dec_process(dec, &raw, &out);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size <= 65536) {
                uint8_t *bigger = realloc(pcm, out.needed_size);
                if (!bigger) break;
                pcm = bigger;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_CONTINUE) break;
            if (out.decoded_size >= 2) {
                esp_audio_simple_dec_info_t info = {0};
                esp_audio_simple_dec_get_info(dec, &info);
                int rate = info.sample_rate ? (int)info.sample_rate : 16000;
                watch_audio_write((int16_t *)pcm, (int)(out.decoded_size / 2), rate);
            }
            if (raw.consumed == 0) break;
        }
    }
    free(in);
    free(pcm);
    esp_audio_simple_dec_close(dec);
    fclose(f);

done:
    free(path);
    s_busy = false;
    s_current[0] = '\0';
    s_task = NULL;
    vTaskDelete(NULL);
}

void watch_media_init(void)
{
    /* Register decoders only; I2S is initialized lazily on first playback. */
    esp_audio_dec_register_default();
}

bool watch_media_play(const char *filename)
{
    if (!filename) return false;
    if (s_busy) {
        s_stop = true;
        for (int i = 0; i < 20 && s_busy; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_busy) return false;
    }
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", WS_MUSIC_DIR, filename);
    char *copy = strdup(path);
    if (!copy) return false;
    s_busy = true;
    s_stop = false;
    snprintf(s_current, sizeof(s_current), "%s", filename);
    if (xTaskCreatePinnedToCore(media_task, "media", 8192, copy, 6, &s_task, 1) != pdPASS) {
        free(copy);
        s_busy = false;
        s_current[0] = '\0';
        return false;
    }
    return true;
}

void watch_media_stop(void)
{
    s_stop = true;
}

bool watch_media_busy(void) { return s_busy; }
const char *watch_media_current(void) { return s_current; }
