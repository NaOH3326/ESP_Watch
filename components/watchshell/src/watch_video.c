#include "watch_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "watch_audio.h"
#include "watch_config.h"
#include "watch_gfx.h"
#include "watch_net.h"

static const char *TAG = "WATCH_VIDEO";

#define VIDEO_PORT        3333
#define VIDEO_MAX_JPEG    (256 * 1024)
#define VIDEO_MAX_PCM     (8 * 1024)
#define VIDEO_MAX_DECODE  (1024 * 1024)
#define VIDEO_AUDIO_QUEUE_LEN 8
#define VIDEO_MAGIC       "WSV2"
#define VIDEO_MAGIC_LEN   4
#define VIDEO_HEADER_LEN  20
#define VIDEO_TYPE_JPEG   1
#define VIDEO_TYPE_PCM16  2

#define VIDEO_CATALOG_MAX      16
#define VIDEO_CATALOG_BODY_MAX (48 * 1024)
#define VIDEO_HTTP_TIMEOUT_MS  8000

static volatile bool s_enabled;
static volatile bool s_connected;
static volatile int s_listen = -1;
static volatile int s_client = -1;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static uint8_t *s_jpeg;
static uint8_t *s_pcm;
static QueueHandle_t s_audio_q;
static TaskHandle_t s_audio_task;

typedef struct {
    uint8_t *data;
    uint16_t len;
} video_audio_pkt_t;

static uint16_t *s_frame;
static int s_frame_w;
static int s_frame_h;
static unsigned s_frames;
static unsigned s_audio_chunks;
static int s_fps;
static int s_fps_count;
static int64_t s_fps_start;

/* Catalog + HTTP pull state.  s_play_gen invalidates a task after STOP or a
 * new selection without trying to free an HTTP client under its feet. */
static ws_video_state_t s_state = WS_VIDEO_OFFLINE;
static ws_video_item_t s_catalog[VIDEO_CATALOG_MAX];
static int s_catalog_count;
static char s_detail[64] = "idle";
static char s_current_title[48];
static volatile int s_play_gen;
static volatile bool s_fetching;
static volatile bool s_streaming;
static volatile int s_fetch_gen;
static TaskHandle_t s_fetch_task;
static TaskHandle_t s_stream_task;
static char s_catalog_url[192];
static char s_catalog_token[192];

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *video_state_name(ws_video_state_t state)
{
    switch (state) {
    case WS_VIDEO_FETCHING: return "FETCHING";
    case WS_VIDEO_READY: return "READY";
    case WS_VIDEO_CONNECTING: return "CONNECTING";
    case WS_VIDEO_PLAYING: return "PLAYING";
    case WS_VIDEO_ERROR: return "ERROR";
    default: return "OFFLINE";
    }
}

const char *watch_video_state_name(ws_video_state_t state)
{
    return video_state_name(state);
}

static void set_state(ws_video_state_t state, const char *detail)
{
    if (!s_lock) {
        s_state = state;
        snprintf(s_detail, sizeof(s_detail), "%s", detail ? detail : "");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    snprintf(s_detail, sizeof(s_detail), "%s", detail ? detail : "");
    xSemaphoreGive(s_lock);
}

static bool url_has_http_scheme(const char *url)
{
    return url && (!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8));
}

/* Copy scheme://host[:port] from an absolute URL. */
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

/* Catalog entries may use absolute URLs or paths.  Resolve "/stream" against
 * the catalog origin and relative refs against the catalog directory. */
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

static void fill_device_id(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_len, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_http_client_handle_t http_open(const char *url, char *err, size_t err_len)
{
    if (err && err_len) err[0] = '\0';
    if (!url_has_http_scheme(url)) {
        snprintf(err, err_len, "bad stream url");
        return NULL;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = VIDEO_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(err, err_len, "http init");
        return NULL;
    }

    esp_http_client_set_header(client, "Accept", "application/vnd.wsv2, application/octet-stream");
    char device[16];
    fill_device_id(device, sizeof(device));
    esp_http_client_set_header(client, "X-Device-Id", device);
    if (s_catalog_token[0]) {
        char auth[224];
        snprintf(auth, sizeof(auth), "Bearer %s", s_catalog_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    if (esp_http_client_open(client, 0) != ESP_OK ||
        esp_http_client_fetch_headers(client) < 0) {
        snprintf(err, err_len, "connect failed");
        esp_http_client_cleanup(client);
        return NULL;
    }
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        snprintf(err, err_len, "http %d", status);
        esp_http_client_cleanup(client);
        return NULL;
    }
    return client;
}

static bool http_read_exact(esp_http_client_handle_t client, uint8_t *buf, size_t len,
                            volatile int gen)
{
    size_t done = 0;
    while (done < len && s_enabled && s_play_gen == gen) {
        int got = esp_http_client_read(client, (char *)buf + done, (int)(len - done));
        if (got <= 0) return false;
        done += (size_t)got;
    }
    return done == len;
}

static bool recv_all(int fd, uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len && s_enabled) {
        int got = recv(fd, buf + done, len - done, 0);
        if (got <= 0) return false;
        done += (size_t)got;
    }
    return done == len;
}

/* The sender is responsible for encoding/scaling.  This fallback scale loop
 * only protects the receiver from a sender that ignores the recommendation. */
static uint16_t *decode_frame(const uint8_t *jpg, size_t len, int *out_w, int *out_h)
{
    static const esp_jpeg_image_scale_t scales[] = {
        JPEG_IMAGE_SCALE_0,
        JPEG_IMAGE_SCALE_1_2,
        JPEG_IMAGE_SCALE_1_4,
        JPEG_IMAGE_SCALE_1_8,
    };
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpg,
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
        if (info.output_len > VIDEO_MAX_DECODE) continue;

        uint16_t *buf = heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = malloc(info.output_len);
        if (!buf) continue;

        cfg.outbuf = (uint8_t *)buf;
        cfg.outbuf_size = (uint32_t)info.output_len;
        if (esp_jpeg_decode(&cfg, &info) == ESP_OK) {
            *out_w = info.width;
            *out_h = info.height;
            return buf;
        }
        free(buf);
    }
    return NULL;
}

static void publish_frame(uint16_t *buf, int w, int h);

static void video_audio_task(void *arg)
{
    (void)arg;
    video_audio_pkt_t pkt;
    while (1) {
        if (xQueueReceive(s_audio_q, &pkt, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (s_enabled && pkt.data && pkt.len >= 2 && !(pkt.len & 1)) {
            if (watch_audio_write((const int16_t *)pkt.data, (int)(pkt.len / 2),
                                  WS_AUDIO_RATE) != ESP_OK) {
                ESP_LOGW(TAG, "audio write failed");
            }
        }
        free(pkt.data);
    }
}

static bool process_frame(uint8_t type, uint16_t sw, uint16_t sh,
                          const uint8_t *data, uint32_t len)
{
    if (type == VIDEO_TYPE_JPEG) {
        int w = 0, h = 0;
        uint16_t *frame = decode_frame(data, len, &w, &h);
        if (frame && s_enabled) {
            publish_frame(frame, w, h);
        } else {
            free(frame);
            if (!frame) {
                ESP_LOGW(TAG, "JPEG decode failed (%ux%u, %u bytes)",
                         sw, sh, (unsigned)len);
            }
        }
        return true;
    }
    if (type == VIDEO_TYPE_PCM16) {
        if ((len & 1) || len > VIDEO_MAX_PCM) return false;
        if (s_audio_q) {
            uint8_t *copy = malloc(len);
            if (copy) {
                memcpy(copy, data, len);
                video_audio_pkt_t pkt = { .data = copy, .len = (uint16_t)len };
                if (xQueueSend(s_audio_q, &pkt, 0) != pdTRUE) {
                    free(copy);
                }
            }
        } else if (watch_audio_write((const int16_t *)data, (int)(len / 2),
                                     WS_AUDIO_RATE) != ESP_OK) {
            ESP_LOGW(TAG, "audio write failed");
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_audio_chunks++;
        xSemaphoreGive(s_lock);
        return true;
    }
    ESP_LOGW(TAG, "unknown frame type %u", type);
    return false;
}

static void publish_frame(uint16_t *buf, int w, int h)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_frame);
    s_frame = buf;
    s_frame_w = w;
    s_frame_h = h;
    s_frames++;
    s_fps_count++;
    if (s_fps_start == 0) {
        s_fps_start = now;
    } else if (now - s_fps_start >= 1000000) {
        s_fps = (int)((int64_t)s_fps_count * 1000000 / (now - s_fps_start));
        s_fps_count = 0;
        s_fps_start = now;
    }
    xSemaphoreGive(s_lock);
}

static void video_client(int fd)
{
    uint8_t hdr[VIDEO_HEADER_LEN];
    s_connected = true;
    ESP_LOGI(TAG, "video sender connected");

    while (s_enabled && recv_all(fd, hdr, sizeof(hdr))) {
        if (memcmp(hdr, VIDEO_MAGIC, VIDEO_MAGIC_LEN) != 0) {
            ESP_LOGW(TAG, "bad frame magic");
            break;
        }
        uint8_t type = hdr[4];
        uint16_t sw = le16(hdr + 6);
        uint16_t sh = le16(hdr + 8);
        uint32_t len = le32(hdr + 16);
        if (!len) break;

        uint8_t *buf = NULL;
        if (type == VIDEO_TYPE_JPEG && len <= VIDEO_MAX_JPEG) {
            buf = s_jpeg;
        } else if (type == VIDEO_TYPE_PCM16 && len <= VIDEO_MAX_PCM && !(len & 1)) {
            buf = s_pcm;
        } else {
            ESP_LOGW(TAG, "bad frame type/size %u/%u", type, (unsigned)len);
            break;
        }
        if (!recv_all(fd, buf, len) || !process_frame(type, sw, sh, buf, len)) {
            break;
        }
    }

    s_connected = false;
    ESP_LOGI(TAG, "video sender disconnected");
}

static bool parse_catalog(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    cJSON *items = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return false;
    }

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_catalog_count = 0;
    int count = 0;
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (count >= VIDEO_CATALOG_MAX) break;
        if (!cJSON_IsObject(it)) continue;

        cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(it, "title");
        cJSON *jurl = cJSON_GetObjectItemCaseSensitive(it, "stream_url");
        if (!cJSON_IsString(jurl)) jurl = cJSON_GetObjectItemCaseSensitive(it, "url");
        if (!cJSON_IsString(jurl) || !jurl->valuestring[0]) continue;

        ws_video_item_t *dst = &s_catalog[count];
        memset(dst, 0, sizeof(*dst));
        snprintf(dst->id, sizeof(dst->id), "%s",
                 cJSON_IsString(jid) ? jid->valuestring : "");
        snprintf(dst->title, sizeof(dst->title), "%s",
                 cJSON_IsString(jtitle) && jtitle->valuestring[0] ?
                 jtitle->valuestring : (dst->id[0] ? dst->id : "VIDEO"));

        if (resolve_catalog_url(s_catalog_url, jurl->valuestring,
                                dst->stream_url, sizeof(dst->stream_url))) {
            cJSON *jlen = cJSON_GetObjectItemCaseSensitive(it, "duration_ms");
            cJSON *jw = cJSON_GetObjectItemCaseSensitive(it, "width");
            cJSON *jh = cJSON_GetObjectItemCaseSensitive(it, "height");
            dst->duration_ms = cJSON_IsNumber(jlen) ? jlen->valueint : 0;
            dst->width = cJSON_IsNumber(jw) ? jw->valueint : 0;
            dst->height = cJSON_IsNumber(jh) ? jh->valueint : 0;
            count++;
        }
    }
    s_catalog_count = count;
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
    return count > 0;
}

static void fetch_task(void *arg)
{
    int gen = (int)(intptr_t)arg;
    char err[64] = {0};
    char url[192];
    char token[192];

    watch_config_get_video(url, sizeof(url), token, sizeof(token));
    if (!url[0]) {
        if (s_enabled && s_fetch_gen == gen) set_state(WS_VIDEO_ERROR, "set video url");
        s_fetching = false;
        s_fetch_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    snprintf(s_catalog_url, sizeof(s_catalog_url), "%s", url);
    snprintf(s_catalog_token, sizeof(s_catalog_token), "%s", token);
    set_state(WS_VIDEO_FETCHING, "fetching catalog");

    esp_http_client_handle_t client = http_open(url, err, sizeof(err));
    if (!client) {
        if (s_enabled && s_fetch_gen == gen) set_state(WS_VIDEO_ERROR, err);
        s_fetching = false;
        s_fetch_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    char *body = heap_caps_malloc(VIDEO_CATALOG_BODY_MAX + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = malloc(VIDEO_CATALOG_BODY_MAX + 1);
    int total = 0;
    if (body) {
        int content_len = esp_http_client_get_content_length(client);
        while (total < VIDEO_CATALOG_BODY_MAX && s_enabled && s_fetch_gen == gen) {
            int want = VIDEO_CATALOG_BODY_MAX - total;
            int got = esp_http_client_read(client, body + total, want);
            if (got <= 0) break;
            total += got;
        }
        if (content_len > VIDEO_CATALOG_BODY_MAX) {
            snprintf(err, sizeof(err), "catalog too large");
            total = 0;
        }
        body[total] = '\0';
    } else {
        snprintf(err, sizeof(err), "no catalog buffer");
    }
    esp_http_client_cleanup(client);

    if (!body) {
        if (s_enabled && s_fetch_gen == gen) set_state(WS_VIDEO_ERROR, err);
    } else if (!s_enabled || s_fetch_gen != gen) {
        /* A newer refresh or leaving the app invalidated this request. */
    } else if (total <= 0) {
        set_state(WS_VIDEO_ERROR, "empty catalog");
    } else if (!parse_catalog(body)) {
        set_state(WS_VIDEO_ERROR, "bad catalog json");
    } else {
        char detail[64];
        snprintf(detail, sizeof(detail), "%d video(s)", s_catalog_count);
        set_state(WS_VIDEO_READY, detail);
        ESP_LOGI(TAG, "catalog: %d item(s)", s_catalog_count);
    }
    free(body);
    s_fetching = false;
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

static void stream_task(void *arg)
{
    int index = (int)(intptr_t)arg;
    int gen = s_play_gen;
    ws_video_item_t item;
    char err[64] = {0};

    memset(&item, 0, sizeof(item));
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index >= 0 && index < s_catalog_count) item = s_catalog[index];
    if (s_lock) xSemaphoreGive(s_lock);
    if (!item.stream_url[0]) {
        set_state(WS_VIDEO_ERROR, "bad selection");
        s_stream_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    snprintf(s_current_title, sizeof(s_current_title), "%s", item.title);
    set_state(WS_VIDEO_CONNECTING, "connecting");
    esp_http_client_handle_t client = http_open(item.stream_url, err, sizeof(err));
    if (!client) {
        if (s_enabled && s_play_gen == gen) set_state(WS_VIDEO_ERROR, err);
        s_stream_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (!s_enabled || s_play_gen != gen) {
        esp_http_client_cleanup(client);
        s_stream_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_streaming = true;
    s_connected = true;
    set_state(WS_VIDEO_PLAYING, item.title);
    ESP_LOGI(TAG, "streaming %s", item.stream_url);

    uint8_t hdr[VIDEO_HEADER_LEN];
    while (s_enabled && s_play_gen == gen &&
           http_read_exact(client, hdr, sizeof(hdr), gen)) {
        if (memcmp(hdr, VIDEO_MAGIC, VIDEO_MAGIC_LEN) != 0) {
            ESP_LOGW(TAG, "bad stream magic");
            break;
        }
        uint8_t type = hdr[4];
        uint16_t sw = le16(hdr + 6);
        uint16_t sh = le16(hdr + 8);
        uint32_t len = le32(hdr + 16);
        if (!len) break;

        uint8_t *buf = NULL;
        if (type == VIDEO_TYPE_JPEG && len <= VIDEO_MAX_JPEG) {
            buf = s_jpeg;
        } else if (type == VIDEO_TYPE_PCM16 && len <= VIDEO_MAX_PCM && !(len & 1)) {
            buf = s_pcm;
        } else {
            ESP_LOGW(TAG, "bad stream frame %u/%u", type, (unsigned)len);
            break;
        }
        if (!http_read_exact(client, buf, len, gen) ||
            !process_frame(type, sw, sh, buf, len)) {
            break;
        }
    }

    esp_http_client_cleanup(client);
    if (s_play_gen == gen) {
        s_streaming = false;
        s_connected = false;
        if (s_enabled) set_state(WS_VIDEO_READY, "stream ended");
    }
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

static void video_task(void *arg)
{
    (void)arg;
    if (!s_jpeg) {
        s_jpeg = heap_caps_malloc(VIDEO_MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_jpeg) s_jpeg = malloc(VIDEO_MAX_JPEG);
    }
    if (!s_pcm) {
        s_pcm = heap_caps_malloc(VIDEO_MAX_PCM, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_pcm) s_pcm = heap_caps_malloc(VIDEO_MAX_PCM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_jpeg || !s_pcm) {
        ESP_LOGE(TAG, "no AV receive buffer");
        free(s_jpeg);
        free(s_pcm);
        s_jpeg = NULL;
        s_pcm = NULL;
        s_task = NULL;
        s_enabled = false;
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int yes = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(VIDEO_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(lfd, 1) != 0) {
            close(lfd);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_listen = lfd;
        ESP_LOGI(TAG, "listening on %s:%d", watch_net_ip(), VIDEO_PORT);

        while (1) {
            if (!s_enabled) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(lfd, &rfds);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
            int ready = select(lfd + 1, &rfds, NULL, NULL, &tv);
            if (ready <= 0) continue;

            int c = accept(lfd, NULL, NULL);
            if (c < 0) continue;
            if (s_streaming) {
                /* A catalog pull stream owns the decoder/audio queue. */
                close(c);
                continue;
            }
            s_client = c;
            struct timeval rcv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
            video_client(c);
            shutdown(c, SHUT_RDWR);
            close(c);
            s_client = -1;
        }

        close(lfd);
        s_listen = -1;
    }
}

void watch_video_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) return;

    if (!s_audio_q) {
        s_audio_q = xQueueCreate(VIDEO_AUDIO_QUEUE_LEN, sizeof(video_audio_pkt_t));
    }
    if (s_audio_q && !s_audio_task) {
        if (xTaskCreatePinnedToCore(video_audio_task, "video_aud", 8192, NULL,
                                    4, &s_audio_task, 1) != pdPASS) {
            s_audio_task = NULL;
            vQueueDelete(s_audio_q);
            s_audio_q = NULL;
        }
    }

    if (!s_jpeg) {
        s_jpeg = heap_caps_malloc(VIDEO_MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_jpeg) s_jpeg = malloc(VIDEO_MAX_JPEG);
    }
    if (!s_pcm) {
        s_pcm = heap_caps_malloc(VIDEO_MAX_PCM, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_pcm) s_pcm = heap_caps_malloc(VIDEO_MAX_PCM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_jpeg || !s_pcm) {
        ESP_LOGE(TAG, "no AV receive buffer");
        s_enabled = false;
        set_state(WS_VIDEO_ERROR, "no av buffer");
        return;
    }

    s_enabled = true;
    s_connected = false;
    if (!s_task) {
        if (xTaskCreatePinnedToCore(video_task, "video", 12288, NULL, 3, &s_task, 0) != pdPASS) {
            s_task = NULL;
            s_enabled = false;
            set_state(WS_VIDEO_ERROR, "no video task");
            ESP_LOGE(TAG, "failed to create video task");
            return;
        }
    }
    if (s_state == WS_VIDEO_OFFLINE) set_state(WS_VIDEO_READY, "ready");
    ESP_LOGI(TAG, "video receiver enabled");
}

void watch_video_stop(void)
{
    s_enabled = false;
    s_connected = false;
    s_streaming = false;
    s_fetching = false;
    s_play_gen++;
    s_fetch_gen++;
    if (s_audio_q) {
        video_audio_pkt_t pkt;
        while (xQueueReceive(s_audio_q, &pkt, 0) == pdTRUE) {
            free(pkt.data);
        }
    }
    int c = s_client;
    if (c >= 0) {
        shutdown(c, SHUT_RDWR);
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_frame);
        s_frame = NULL;
        s_frame_w = s_frame_h = 0;
        s_fps = 0;
        s_fps_count = 0;
        s_fps_start = 0;
        s_audio_chunks = 0;
        s_state = WS_VIDEO_OFFLINE;
        s_detail[0] = '\0';
        s_current_title[0] = '\0';
        s_catalog_count = 0;
        memset(s_catalog, 0, sizeof(s_catalog));
        xSemaphoreGive(s_lock);
    } else {
        s_state = WS_VIDEO_OFFLINE;
        s_detail[0] = '\0';
        s_current_title[0] = '\0';
        s_catalog_count = 0;
    }
}

void watch_video_get_status(ws_video_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->running = s_enabled;
    out->connected = s_connected;
    out->port = VIDEO_PORT;
    snprintf(out->ip, sizeof(out->ip), "%s", watch_net_ip());
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        out->has_frame = s_frame != NULL;
        out->width = s_frame_w;
        out->height = s_frame_h;
        out->frames = s_frames;
        out->audio_chunks = s_audio_chunks;
        out->fps = s_fps;
        out->state = s_state;
        out->catalog_count = s_catalog_count;
        snprintf(out->title, sizeof(out->title), "%s", s_current_title);
        snprintf(out->detail, sizeof(out->detail), "%s", s_detail);
        xSemaphoreGive(s_lock);
    }
}

static void catalog_fetch_if_enabled(void)
{
    if (!s_enabled || s_fetching || s_streaming) return;
    s_fetch_gen++;
    int gen = s_fetch_gen;
    s_fetching = true;
    if (xTaskCreatePinnedToCore(fetch_task, "video_cat", 12288,
                                (void *)(intptr_t)gen, 3, &s_fetch_task, 0) != pdPASS) {
        s_fetching = false;
        s_fetch_task = NULL;
        set_state(WS_VIDEO_ERROR, "no catalog task");
    }
}

void watch_video_catalog_fetch(void)
{
    if (s_streaming) watch_video_stop_playback();
    catalog_fetch_if_enabled();
}

int watch_video_catalog_count(void)
{
    int n = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        n = s_catalog_count;
        xSemaphoreGive(s_lock);
    }
    return n;
}

bool watch_video_catalog_get(int index, ws_video_item_t *out)
{
    if (!out || index < 0) return false;
    bool ok = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (index < s_catalog_count) {
            *out = s_catalog[index];
            ok = true;
        }
        xSemaphoreGive(s_lock);
    }
    return ok;
}

void watch_video_stop_playback(void)
{
    s_play_gen++;
    s_streaming = false;
    s_connected = false;
    int c = s_client;
    if (c >= 0) {
        shutdown(c, SHUT_RDWR);
    }
    if (s_enabled) set_state(WS_VIDEO_READY, "stopped");
}

void watch_video_play(int index)
{
    if (!s_enabled || s_fetching) return;
    if (index < 0 || index >= watch_video_catalog_count()) {
        set_state(WS_VIDEO_ERROR, "no video selected");
        return;
    }
    watch_video_stop_playback();
    s_play_gen++;
    set_state(WS_VIDEO_CONNECTING, "connecting");
    if (xTaskCreatePinnedToCore(stream_task, "video_http", 12288,
                                (void *)(intptr_t)index, 3, &s_stream_task, 0) != pdPASS) {
        s_stream_task = NULL;
        set_state(WS_VIDEO_ERROR, "no stream task");
    }
}

bool watch_video_blit(int x, int y, int max_w, int max_h)
{
    if (!s_lock || max_w <= 0 || max_h <= 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = false;
    if (s_frame && s_frame_w > 0 && s_frame_h > 0) {
        int w = max_w, h = max_h;
        if ((int64_t)s_frame_w * max_h > (int64_t)s_frame_h * max_w) {
            h = s_frame_h * max_w / s_frame_w;
        } else {
            w = s_frame_w * max_h / s_frame_h;
        }
        int dx = x + (max_w - w) / 2;
        int dy = y + (max_h - h) / 2;
        watch_gfx_blit_scaled(dx, dy, w, h, s_frame, s_frame_w, s_frame_h);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool watch_video_blit_cover(int x, int y, int max_w, int max_h)
{
    if (!s_lock || max_w <= 0 || max_h <= 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = false;
    if (s_frame && s_frame_w > 0 && s_frame_h > 0) {
        int w = max_w, h = max_h;
        if ((int64_t)s_frame_w * max_h > (int64_t)s_frame_h * max_w) {
            w = s_frame_w * max_h / s_frame_h;
        } else {
            h = s_frame_h * max_w / s_frame_w;
        }
        int dx = x + (max_w - w) / 2;
        int dy = y + (max_h - h) / 2;
        watch_gfx_blit_scaled(dx, dy, w, h, s_frame, s_frame_w, s_frame_h);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}
