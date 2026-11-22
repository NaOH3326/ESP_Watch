#include "watch_media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "watch_audio.h"
#include "watch_config.h"
#include "watch_storage.h"

static const char *TAG = "WATCH_MEDIA";

#define MUSIC_CATALOG_BODY_MAX (48 * 1024)
#define MUSIC_HTTP_TIMEOUT_MS  20000
#define MUSIC_INPUT_CHUNK      4096
#define MUSIC_PCM_CAP          (16 * 1024)
#define MUSIC_MONO_CAP         (32 * 1024)
#define MUSIC_PCM_MAX          (256 * 1024)

static volatile bool s_busy;
static volatile bool s_stop;
static char s_current[48];
static TaskHandle_t s_task;

static SemaphoreHandle_t s_catalog_lock;
static ws_music_item_t s_catalog[WS_MUSIC_MAX_ITEMS];
static int s_catalog_count;
static char s_catalog_url[192];
static char s_catalog_token[192];
static volatile bool s_fetching;
static volatile int s_fetch_gen;
static TaskHandle_t s_fetch_task;

typedef struct {
    bool remote;
    char *source;
    char format[8];
    char title[64];
} media_job_t;

typedef struct {
    FILE *file;
    esp_http_client_handle_t http;
} media_input_t;

static bool suffix(const char *s, const char *e)
{
    if (!s || !e) return false;
    size_t n = strlen(s), m = strlen(e);
    return n >= m && !strcasecmp(s + n - m, e);
}

static bool url_has_http_scheme(const char *url)
{
    return url && (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8));
}

static bool url_origin(const char *url, char *out, size_t out_len)
{
    if (!url_has_http_scheme(url) || !out || out_len < 8) return false;
    const char *p = strstr(url, "://");
    const char *slash = strchr(p + 3, '/');
    size_t len = slash ? (size_t)(slash - url) : strlen(url);
    if (len >= out_len) return false;
    memcpy(out, url, len);
    out[len] = '\0';
    return true;
}

static bool resolve_catalog_url(const char *base, const char *ref, char *out, size_t out_len)
{
    if (!ref || !ref[0] || !out || out_len < 8) return false;
    if (url_has_http_scheme(ref)) {
        snprintf(out, out_len, "%s", ref);
        return true;
    }
    char origin[160];
    if (!url_origin(base, origin, sizeof(origin))) return false;
    if (ref[0] == '/') {
        snprintf(out, out_len, "%s%s", origin, ref);
        return true;
    }
    const char *p = strstr(base, "://");
    const char *slash = p ? strchr(p + 3, '/') : NULL;
    const char *last = slash ? strrchr(slash, '/') : NULL;
    if (!last) {
        snprintf(out, out_len, "%s/%s", base, ref);
        return true;
    }
    size_t prefix = (size_t)(last - base) + 1;
    if (prefix >= out_len) return false;
    memcpy(out, base, prefix);
    snprintf(out + prefix, out_len - prefix, "%s", ref);
    return true;
}

static esp_audio_simple_dec_type_t decoder_type(const char *format)
{
    if (suffix(format, "wav")) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (suffix(format, "m4a")) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (suffix(format, "aac")) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

/* Decoders return PCM in their native layout: MP3/AAC are usually 16-bit
 * stereo, WAV may be 8/16/24/32-bit.  watch_audio_write() expects mono
 * int16_t, so convert here instead of treating interleaved stereo bytes as
 * mono samples (which produces silence/noise at the wrong rate). */
static int pcm_to_mono16(const uint8_t *src, uint32_t bytes, int channels,
                         int bits, int16_t *dst, size_t dst_samples)
{
    if (!src || !dst || bytes == 0 || dst_samples == 0) return 0;
    if (channels < 1) channels = 1;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) bits = 16;
    int sample_bytes = bits / 8;
    uint32_t frame_bytes = (uint32_t)sample_bytes * (uint32_t)channels;
    uint32_t frames = bytes / frame_bytes;
    if (frames > dst_samples) frames = (uint32_t)dst_samples;

    for (uint32_t i = 0; i < frames; i++) {
        int32_t sum = 0;
        for (int c = 0; c < channels; c++) {
            const uint8_t *p = src + (size_t)i * frame_bytes +
                               (size_t)c * sample_bytes;
            int32_t s = 0;
            if (bits == 8) {
                s = ((int32_t)p[0] - 128) << 8;
            } else if (bits == 16) {
                s = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            } else if (bits == 24) {
                uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                             ((uint32_t)p[2] << 16);
                if (u & 0x800000u) u |= 0xff000000u;
                s = (int32_t)u >> 8;
            } else {
                uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                s = (int32_t)u >> 16;
            }
            sum += s;
        }
        dst[i] = (int16_t)(sum / channels);
    }
    return (int)frames;
}

static esp_http_client_handle_t http_open(const char *url, const char *accept)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = MUSIC_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "http init failed");
        return NULL;
    }
    if (s_catalog_token[0]) {
        char auth[224];
        snprintf(auth, sizeof(auth), "Bearer %s", s_catalog_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_header(client, "Accept", accept);
    if (esp_http_client_open(client, 0) != ESP_OK ||
        esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_cleanup(client);
        ESP_LOGW(TAG, "http connect failed: %s", url);
        return NULL;
    }
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "http status %d: %s", status, url);
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

static int input_read(media_input_t *in, uint8_t *buf, int len)
{
    if (in->http) return esp_http_client_read(in->http, (char *)buf, len);
    if (in->file) return (int)fread(buf, 1, (size_t)len, in->file);
    return -1;
}

static void media_task(void *arg)
{
    media_job_t *job = (media_job_t *)arg;
    media_input_t in = {0};
    esp_audio_simple_dec_handle_t dec = NULL;
    uint8_t *inbuf = NULL;
    uint8_t *pcm = NULL;
    int16_t *mono = NULL;
    size_t pcm_cap = MUSIC_PCM_CAP;
    size_t mono_cap = MUSIC_MONO_CAP;
    bool failed = false;

    if (job->remote) {
        ESP_LOGI(TAG, "remote stream start: %s", job->title);
        in.http = http_open(job->source, "audio/*,application/octet-stream");
        if (!in.http) goto done;
        ESP_LOGI(TAG, "remote HTTP %d, content-length=%lld",
                 esp_http_client_get_status_code(in.http),
                 (long long)esp_http_client_get_content_length(in.http));
    } else {
        in.file = fopen(job->source, "rb");
        if (!in.file) {
            ESP_LOGE(TAG, "open %s failed", job->source);
            goto done;
        }
    }

    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = decoder_type(job->format[0] ? job->format : job->source),
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_err_t ret = esp_audio_simple_dec_open(&cfg, &dec);
    if (ret != ESP_AUDIO_ERR_OK || !dec) {
        ESP_LOGE(TAG, "decoder open failed: %d", ret);
        goto done;
    }
    inbuf = malloc(MUSIC_INPUT_CHUNK);
    pcm = malloc(pcm_cap);
    mono = malloc(mono_cap);
    if (!inbuf || !pcm || !mono) {
        ESP_LOGE(TAG, "no decode buffer");
        goto done;
    }

    bool info_ready = false;
    bool first_written = false;
    bool write_warned = false;
    uint32_t rate = 16000;
    int channels = 1;
    int bits = 16;
    ESP_LOGI(TAG, "audio volume %d%%", watch_audio_volume());

    while (!s_stop) {
        int got = input_read(&in, inbuf, MUSIC_INPUT_CHUNK);
        if (got <= 0) {
            if (in.http) {
                ESP_LOGI(TAG, "stream read end: %d, complete=%d, status=%d",
                         got, (int)esp_http_client_is_complete_data_received(in.http),
                         esp_http_client_get_status_code(in.http));
            } else if (got < 0) {
                ESP_LOGW(TAG, "file read failed: %s", job->source);
            }
            break;
        }
        bool eos = in.file && got < MUSIC_INPUT_CHUNK;
        if (in.http) eos = esp_http_client_is_complete_data_received(in.http);
        esp_audio_simple_dec_raw_t raw = {
            .buffer = inbuf,
            .len = (uint32_t)got,
            .eos = eos,
        };
        /* Do not overwrite inbuf until this loop has consumed it: the decoder
         * may keep a frame cached when it returns less than `got`. */
        while (raw.len > 0 && !s_stop) {
            esp_audio_simple_dec_out_t out = {
                .buffer = pcm,
                .len = (uint32_t)pcm_cap,
            };
            ret = esp_audio_simple_dec_process(dec, &raw, &out);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint32_t need = out.needed_size;
                if (need < pcm_cap * 2) need = (uint32_t)pcm_cap * 2;
                if (need > MUSIC_PCM_MAX) {
                    ESP_LOGE(TAG, "decoded frame too large: %u", (unsigned)need);
                    failed = true;
                    break;
                }
                uint8_t *bigger = realloc(pcm, need);
                if (!bigger) {
                    ESP_LOGE(TAG, "pcm realloc %u failed", (unsigned)need);
                    failed = true;
                    break;
                }
                pcm = bigger;
                int16_t *bigger_mono = realloc(mono, (size_t)need * 2);
                if (!bigger_mono) {
                    ESP_LOGE(TAG, "mono realloc %u failed", (unsigned)(need * 2));
                    failed = true;
                    break;
                }
                mono = bigger_mono;
                pcm_cap = need;
                mono_cap = (size_t)need * 2;
                continue;
            }
            if (ret == ESP_AUDIO_ERR_DATA_LACK) break;
            if (ret != ESP_AUDIO_ERR_OK && ret != ESP_AUDIO_ERR_CONTINUE) {
                ESP_LOGW(TAG, "decode error %d (consumed=%u, in=%u)",
                         ret, (unsigned)raw.consumed, (unsigned)raw.len);
                failed = true;
                break;
            }
            if (out.decoded_size > 0) {
                esp_audio_simple_dec_info_t info = {0};
                esp_audio_simple_dec_get_info(dec, &info);
                if (!info_ready) {
                    if (info.sample_rate >= 8000 && info.sample_rate <= 192000) {
                        rate = info.sample_rate;
                    }
                    if (info.channel >= 1 && info.channel <= 8) {
                        channels = info.channel;
                    }
                    if (info.bits_per_sample == 8 || info.bits_per_sample == 16 ||
                        info.bits_per_sample == 24 || info.bits_per_sample == 32) {
                        bits = info.bits_per_sample;
                    }
                    info_ready = true;
                    ESP_LOGI(TAG, "decoded audio: %u Hz, %d ch, %d bit, frame=%u",
                             (unsigned)rate, channels, bits,
                             (unsigned)out.decoded_size);
                }

                int samples = pcm_to_mono16(pcm, out.decoded_size, channels, bits,
                                            mono, mono_cap / sizeof(int16_t));
                if (samples > 0) {
                    if (!first_written) {
                        first_written = true;
                        int64_t energy = 0;
                        int peak = 0;
                        for (int i = 0; i < samples; i++) {
                            int v = mono[i];
                            energy += v < 0 ? -v : v;
                            if (v > peak) peak = v;
                            if (-v > peak) peak = -v;
                        }
                        ESP_LOGI(TAG, "first PCM: %d samples, avg=%d, peak=%d",
                                 samples, (int)(energy / samples), peak);
                    }
                    esp_err_t wr = watch_audio_write(mono, samples, (int)rate);
                    if (wr != ESP_OK && !write_warned) {
                        write_warned = true;
                        ESP_LOGW(TAG, "audio write failed: %s", esp_err_to_name(wr));
                    }
                }
            }

            uint32_t used = raw.consumed;
            if (used > raw.len) used = raw.len;
            if (used == 0) break;
            raw.buffer += used;
            raw.len -= used;
        }
        if (failed) break;
    }

    if (job->remote) ESP_LOGI(TAG, "remote stream stopped: %s", job->title);

done:
    if (in.http) esp_http_client_cleanup(in.http);
    if (in.file) fclose(in.file);
    if (dec) esp_audio_simple_dec_close(dec);
    free(inbuf);
    free(pcm);
    free(mono);
    free(job->source);
    free(job);
    s_busy = false;
    s_current[0] = '\0';
    s_task = NULL;
    vTaskDelete(NULL);
}

static bool start_job(media_job_t *job, const char *title)
{
    if (!job || !job->source) {
        if (job) free(job);
        return false;
    }
    s_busy = true;
    s_stop = false;
    snprintf(s_current, sizeof(s_current), "%s", title ? title : "music");
    if (xTaskCreatePinnedToCore(media_task, "media", 12288, job, 6, &s_task, 1) != pdPASS) {
        free(job->source);
        free(job);
        s_busy = false;
        s_current[0] = '\0';
        return false;
    }
    return true;
}

static bool wait_idle(void)
{
    if (!s_busy) return true;
    s_stop = true;
    for (int i = 0; i < 100 && s_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return !s_busy;
}

void watch_media_init(void)
{
    static bool decoders_registered;
    if (!s_catalog_lock) s_catalog_lock = xSemaphoreCreateMutex();
    if (!decoders_registered) {
        decoders_registered = true;
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
    }
}

bool watch_media_play(const char *filename)
{
    if (!filename || !wait_idle()) return false;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", WS_MUSIC_DIR, filename);
    media_job_t *job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->remote = false;
    job->source = strdup(path);
    snprintf(job->title, sizeof(job->title), "%s", filename);
    if (!job->source) {
        free(job);
        return false;
    }
    return start_job(job, filename);
}

bool watch_media_play_remote(int index)
{
    ws_music_item_t item = {0};
    if (!watch_media_catalog_get(index, &item) || !wait_idle()) return false;
    media_job_t *job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->remote = true;
    job->source = strdup(item.url);
    snprintf(job->format, sizeof(job->format), "%s", item.format);
    snprintf(job->title, sizeof(job->title), "%s", item.title);
    if (!job->source) {
        free(job);
        return false;
    }
    return start_job(job, item.title);
}

void watch_media_stop(void)
{
    s_stop = true;
}

bool watch_media_busy(void) { return s_busy; }
const char *watch_media_current(void) { return s_current; }

static bool parse_catalog(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    cJSON *items = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return false;
    }

    if (s_catalog_lock) xSemaphoreTake(s_catalog_lock, portMAX_DELAY);
    s_catalog_count = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (s_catalog_count >= WS_MUSIC_MAX_ITEMS) break;
        if (!cJSON_IsObject(it)) continue;
        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(it, "title");
        cJSON *jurl = cJSON_GetObjectItemCaseSensitive(it, "stream_url");
        if (!cJSON_IsString(jurl)) jurl = cJSON_GetObjectItemCaseSensitive(it, "url");
        if (!cJSON_IsString(jurl) || !jurl->valuestring[0]) continue;

        ws_music_item_t *dst = &s_catalog[s_catalog_count];
        memset(dst, 0, sizeof(*dst));
        snprintf(dst->id, sizeof(dst->id), "%s",
                 cJSON_IsString(jid) ? jid->valuestring : "");
        snprintf(dst->title, sizeof(dst->title), "%s",
                 cJSON_IsString(jtitle) && jtitle->valuestring[0] ?
                 jtitle->valuestring : (dst->id[0] ? dst->id : "MUSIC"));
        cJSON *jfmt = cJSON_GetObjectItemCaseSensitive(it, "format");
        snprintf(dst->format, sizeof(dst->format), "%s",
                 cJSON_IsString(jfmt) ? jfmt->valuestring : "mp3");
        cJSON *jlen = cJSON_GetObjectItemCaseSensitive(it, "duration_ms");
        cJSON *jsize = cJSON_GetObjectItemCaseSensitive(it, "size");
        dst->duration_ms = cJSON_IsNumber(jlen) ? jlen->valueint : 0;
        dst->size = cJSON_IsNumber(jsize) ? jsize->valueint : 0;
        if (resolve_catalog_url(s_catalog_url, jurl->valuestring,
                                dst->url, sizeof(dst->url))) {
            s_catalog_count++;
        }
    }
    int count = s_catalog_count;
    if (s_catalog_lock) xSemaphoreGive(s_catalog_lock);
    cJSON_Delete(root);
    return count > 0;
}

static void fetch_task(void *arg)
{
    int gen = (int)(intptr_t)arg;
    char url[192] = {0};
    char token[192] = {0};

    watch_config_get_music(url, sizeof(url), token, sizeof(token));
    if (!url[0]) {
        if (s_catalog_lock) xSemaphoreTake(s_catalog_lock, portMAX_DELAY);
        s_catalog_count = 0;
        if (s_catalog_lock) xSemaphoreGive(s_catalog_lock);
        s_fetching = false;
        s_fetch_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    snprintf(s_catalog_url, sizeof(s_catalog_url), "%s", url);
    snprintf(s_catalog_token, sizeof(s_catalog_token), "%s", token);

    esp_http_client_handle_t client = http_open(s_catalog_url, "application/json");
    if (!client) {
        s_fetching = false;
        s_fetch_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    char *body = heap_caps_malloc(MUSIC_CATALOG_BODY_MAX + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = malloc(MUSIC_CATALOG_BODY_MAX + 1);
    int total = 0;
    if (body) {
        while (total < MUSIC_CATALOG_BODY_MAX && s_fetch_gen == gen) {
            int got = esp_http_client_read(client, body + total,
                                           MUSIC_CATALOG_BODY_MAX - total);
            if (got <= 0) break;
            total += got;
        }
        body[total] = '\0';
    }
    esp_http_client_cleanup(client);
    if (body && total > 0 && s_fetch_gen == gen) {
        if (parse_catalog(body)) {
            ESP_LOGI(TAG, "music catalog: %d item(s)", watch_media_catalog_count());
        } else {
            ESP_LOGW(TAG, "music catalog parse failed");
        }
    }
    free(body);
    s_fetching = false;
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

void watch_media_catalog_fetch(void)
{
    if (s_fetching) return;
    s_fetch_gen++;
    int gen = s_fetch_gen;
    s_fetching = true;
    if (xTaskCreatePinnedToCore(fetch_task, "music_cat", 12288,
                                (void *)(intptr_t)gen, 2, &s_fetch_task, 0) != pdPASS) {
        s_fetching = false;
        s_fetch_task = NULL;
        ESP_LOGW(TAG, "music catalog task create failed");
    }
}

int watch_media_catalog_count(void)
{
    int n = 0;
    if (s_catalog_lock) xSemaphoreTake(s_catalog_lock, portMAX_DELAY);
    n = s_catalog_count;
    if (s_catalog_lock) xSemaphoreGive(s_catalog_lock);
    return n;
}

bool watch_media_catalog_get(int index, ws_music_item_t *out)
{
    if (!out || index < 0) return false;
    bool ok = false;
    if (s_catalog_lock) xSemaphoreTake(s_catalog_lock, portMAX_DELAY);
    if (index < s_catalog_count) {
        *out = s_catalog[index];
        ok = true;
    }
    if (s_catalog_lock) xSemaphoreGive(s_catalog_lock);
    return ok;
}
