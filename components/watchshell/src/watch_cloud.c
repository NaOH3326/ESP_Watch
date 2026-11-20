#include "watch_cloud.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "watch_audio.h"
#include "watch_config.h"
#include "watch_net.h"

#include "esp_opus_dec.h"
#include "esp_opus_enc.h"

static const char *TAG = "WATCH_AI";
static TaskHandle_t s_ws_task;
static TaskHandle_t s_play_task;
static volatile bool s_stop;
static volatile bool s_connected;
static volatile bool s_listening;
static volatile bool s_stop_sent;
static volatile uint32_t s_capture_gen;
static volatile ws_ai_state_t s_state = WS_AI_OFFLINE;
static volatile int64_t s_state_at;
static char s_url[192];
static char s_token[192];
static char s_host[96];
static char s_path[128];
static int s_port;
static bool s_secure;
static esp_transport_handle_t s_transport;
static SemaphoreHandle_t s_send_lock;
static SemaphoreHandle_t s_info_lock;
static QueueHandle_t s_audio_q;
static int s_tts_rate = 24000;
static char s_user[WS_AI_TEXT_MAX];
static char s_reply[WS_AI_TEXT_MAX];
static char s_detail[64] = "offline";
static char s_live_user[WS_AI_LIVE_TEXT_MAX];
static char s_live_reply[WS_AI_LIVE_TEXT_MAX];
static bool s_live_user_on;
static bool s_live_reply_on;
static ws_ai_chat_msg_t s_chat[WS_AI_CHAT_MAX];
static int s_chat_count;

typedef struct {
    uint8_t *data;
    int len;
} audio_pkt_t;

static void set_state(ws_ai_state_t st, const char *detail)
{
    s_state = st;
    s_state_at = esp_timer_get_time();
    if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
    snprintf(s_detail, sizeof(s_detail), "%s", detail ? detail : "");
    if (s_info_lock) xSemaphoreGive(s_info_lock);
}

static void check_state_timeout(void)
{
    if (s_state == WS_AI_THINKING && s_state_at > 0 &&
        esp_timer_get_time() - s_state_at > 45LL * 1000 * 1000) {
        ESP_LOGW(TAG, "AI response timeout");
        set_state(WS_AI_ERROR, "response timeout");
    }
}

/* Persisted-history helpers assume s_info_lock is already held. */
static int chat_bytes_locked(void)
{
    int bytes = 0;
    for (int i = 0; i < s_chat_count; i++) {
        bytes += (int)strlen(s_chat[i].text);
    }
    return bytes;
}

static void chat_drop_oldest_locked(int count)
{
    if (count <= 0 || s_chat_count <= 0) return;
    if (count > s_chat_count) count = s_chat_count;
    memmove(&s_chat[0], &s_chat[count],
            sizeof(s_chat[0]) * (size_t)(s_chat_count - count));
    s_chat_count -= count;
}

/* Commit a complete user/assistant round only when it fits both the per-message
 * storage limit and the total history budget.  Older rounds are evicted as
 * needed.  If the new round is itself too large, leave it live-only. */
static bool chat_commit_round_locked(const char *user, const char *reply)
{
    if (!user || !reply || !user[0] || !reply[0]) return false;
    size_t ulen = strlen(user);
    size_t rlen = strlen(reply);
    if (ulen >= WS_AI_TEXT_MAX || rlen >= WS_AI_TEXT_MAX) return false;
    int need = (int)(ulen + rlen);
    if (need > WS_AI_CHAT_BUDGET) return false;

    int bytes = chat_bytes_locked();
    while (s_chat_count > 0 &&
           (s_chat_count + 2 > WS_AI_CHAT_MAX ||
            bytes + need > WS_AI_CHAT_BUDGET)) {
        int drop = s_chat_count >= 2 ? 2 : 1;
        chat_drop_oldest_locked(drop);
        bytes = chat_bytes_locked();
    }
    if (s_chat_count + 2 > WS_AI_CHAT_MAX ||
        bytes + need > WS_AI_CHAT_BUDGET) {
        return false;
    }

    ws_ai_chat_msg_t *user_msg = &s_chat[s_chat_count++];
    user_msg->role = WS_AI_ROLE_USER;
    snprintf(user_msg->text, sizeof(user_msg->text), "%s", user);
    ws_ai_chat_msg_t *reply_msg = &s_chat[s_chat_count++];
    reply_msg->role = WS_AI_ROLE_ASSISTANT;
    snprintf(reply_msg->text, sizeof(reply_msg->text), "%s", reply);
    return true;
}

static bool parse_url(const char *url)
{
    if (!url || !url[0]) return false;
    const char *p = strstr(url, "://");
    if (!p) return false;
    size_t scheme_len = (size_t)(p - url);
    s_secure = (scheme_len == 3 && !strncasecmp(url, "wss", 3));
    if (!(scheme_len == 2 && !strncasecmp(url, "ws", 2)) && !s_secure) return false;
    p += 3;
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(host_end - p));
    size_t host_len = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (host_len == 0 || host_len >= sizeof(s_host)) return false;
    memcpy(s_host, p, host_len);
    s_host[host_len] = '\0';
    s_port = s_secure ? 443 : 80;
    if (colon) {
        s_port = atoi(colon + 1);
        if (s_port <= 0 || s_port > 65535) return false;
    }
    snprintf(s_path, sizeof(s_path), "%s", slash ? slash : "/");
    return true;
}

static bool send_text(const char *json)
{
    if (!json) return false;
    int len = (int)strlen(json);
    if (len <= 0) return false;
    /* esp_transport_ws_send_raw() masks the buffer in place.  Never pass a
     * string literal or other read-only memory here: copy into writable RAM. */
    char *copy = malloc((size_t)len);
    if (!copy) {
        ESP_LOGW(TAG, "no memory for text frame");
        return false;
    }
    memcpy(copy, json, (size_t)len);
    if (s_send_lock) xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_transport_handle_t transport = s_transport;
    int ret = -1;
    if (transport && s_connected) {
        ret = esp_transport_ws_send_raw(transport,
                                        WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                                        copy, len, 3000);
    }
    if (s_send_lock) xSemaphoreGive(s_send_lock);
    free(copy);
    if (ret < 0) {
        ESP_LOGW(TAG, "send text failed");
        s_connected = false;
        return false;
    }
    return true;
}

static bool send_bin(const uint8_t *data, int len)
{
    if (!data || len <= 0) return false;
    if (s_send_lock) xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_transport_handle_t transport = s_transport;
    int ret = -1;
    if (transport && s_connected) {
        ret = esp_transport_ws_send_raw(transport,
                                        WS_TRANSPORT_OPCODES_BINARY | WS_TRANSPORT_OPCODES_FIN,
                                        (const char *)data, len, 3000);
    }
    if (s_send_lock) xSemaphoreGive(s_send_lock);
    if (ret < 0) {
        static bool logged_bin_fail;
        if (!logged_bin_fail) {
            logged_bin_fail = true;
            ESP_LOGW(TAG, "send audio failed");
        }
        s_connected = false;
    }
    return ret >= 0;
}

/* Close/destroy under the same lock used by send_text()/send_bin().  Without
 * this, a capture task can be inside esp_transport_ws_send_raw() while the
 * reader task destroys the transport, producing fd=-1 writes and crashes. */
static void close_transport(void)
{
    if (s_send_lock) xSemaphoreTake(s_send_lock, portMAX_DELAY);
    esp_transport_handle_t transport = s_transport;
    s_transport = NULL;
    s_connected = false;
    if (transport) {
        esp_transport_close(transport);
        esp_transport_destroy(transport);
    }
    if (s_send_lock) xSemaphoreGive(s_send_lock);
}

static bool send_listen_stop(void)
{
    if (s_stop_sent) return true;
    if (send_text("{\"type\":\"listen\",\"state\":\"stop\"}")) {
        s_stop_sent = true;
        return true;
    }
    return false;
}

static void audio_queue_clear(void)
{
    if (!s_audio_q) return;
    audio_pkt_t pkt;
    while (xQueueReceive(s_audio_q, &pkt, 0) == pdTRUE) {
        free(pkt.data);
    }
}

static void audio_enqueue(uint8_t *data, int len)
{
    if (!s_audio_q || !data || len <= 0) {
        free(data);
        return;
    }
    audio_pkt_t pkt = { .data = data, .len = len };
    /* Never block the WebSocket reader: while TTS is playing, a full queue
     * must not stop ping/chat/tts-stop text frames from being processed. */
    if (xQueueSend(s_audio_q, &pkt, 0) == pdTRUE) return;

    audio_pkt_t old;
    if (xQueueReceive(s_audio_q, &old, 0) == pdTRUE) {
        free(old.data);
    }
    if (xQueueSend(s_audio_q, &pkt, 0) != pdTRUE) {
        static bool logged_drop;
        if (!logged_drop) {
            logged_drop = true;
            ESP_LOGW(TAG, "TTS audio queue full, dropping frames");
        }
        free(data);
    }
}

static void play_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "ai_play started, stack free %u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    int16_t *pcm = malloc(4096);
    if (!pcm) {
        ESP_LOGE(TAG, "no memory for TTS PCM buffer");
        vTaskDelete(NULL);
        return;
    }
    void *decoder = NULL;
    int dec_rate = 0;
    bool logged_first = false;
    bool logged_first_written = false;
    bool logged_slow_write = false;
    int played_frames = 0;
    while (!s_stop) {
        audio_pkt_t pkt;
        if (xQueueReceive(s_audio_q, &pkt, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (!decoder || dec_rate != s_tts_rate) {
            if (decoder) {
                esp_opus_dec_close(decoder);
                decoder = NULL;
            }
            esp_opus_dec_cfg_t cfg = {
                .sample_rate = (uint32_t)s_tts_rate,
                .channel = 1,
                .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
                .self_delimited = false,
            };
            ESP_LOGI(TAG, "opening opus decoder at %d Hz", s_tts_rate);
            if (esp_opus_dec_open(&cfg, sizeof(cfg), &decoder) != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "opus decoder open failed");
                decoder = NULL;
                free(pkt.data);
                continue;
            }
            dec_rate = s_tts_rate;
            ESP_LOGI(TAG, "opus decoder ready at %d Hz", dec_rate);
        }
        esp_audio_dec_in_raw_t raw = { .buffer = pkt.data, .len = (uint32_t)pkt.len };
        esp_audio_dec_out_frame_t out = { .buffer = (uint8_t *)pcm, .len = 4096 };
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t ret = esp_opus_dec_decode(decoder, &raw, &out, &info);
        if (ret == ESP_AUDIO_ERR_OK && out.decoded_size >= 2) {
            int frames = (int)(out.decoded_size / 2);
            if (!logged_first) {
                logged_first = true;
                ESP_LOGI(TAG, "TTS first frame: %d bytes -> %d samples, stack free %u",
                         pkt.len, frames, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
            int64_t write_start = esp_timer_get_time();
            esp_err_t wr = watch_audio_write(pcm, frames, dec_rate);
            int64_t write_us = esp_timer_get_time() - write_start;
            if (!logged_slow_write && write_us > 250000) {
                logged_slow_write = true;
                ESP_LOGW(TAG, "slow TTS audio write: %lld ms", (long long)(write_us / 1000));
            }
            if (wr != ESP_OK) {
                static bool logged_write_error;
                if (!logged_write_error) {
                    logged_write_error = true;
                    ESP_LOGW(TAG, "TTS audio write failed: %s", esp_err_to_name(wr));
                }
            } else if (!logged_first_written) {
                logged_first_written = true;
                ESP_LOGI(TAG, "TTS first frame written, stack free %u",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
            played_frames++;
            if (played_frames == 2 || (played_frames % 24) == 0) {
                ESP_LOGI(TAG, "TTS played %d frames", played_frames);
            }
        }
        free(pkt.data);
    }
    free(pcm);
    if (decoder) esp_opus_dec_close(decoder);
    vTaskDelete(NULL);
}

static void condition_audio(int16_t *pcm, int n, int *out_gain)
{
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += pcm[i];
    int mean = (int)(sum / n);
    int peak = 0;
    for (int i = 0; i < n; i++) {
        int v = (int)pcm[i] - mean;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
        int a = v < 0 ? -v : v;
        if (a > peak) peak = a;
    }
    /* The MEMS mic is quiet on this board.  Boost only low-level frames so
     * normal speech is closer to full scale; loud frames keep gain=1. */
    int gain = peak < 300 ? 8 : peak < 1000 ? 4 : peak < 3000 ? 2 : 1;
    if (gain > 1) {
        for (int i = 0; i < n; i++) {
            int v = (int)pcm[i] * gain;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            pcm[i] = (int16_t)v;
        }
    }
    if (out_gain) *out_gain = gain;
}

static void log_capture_level(const int16_t *pcm, int n, int gain)
{
    int64_t sum_abs = 0;
    int peak = 0;
    for (int i = 0; i < n; i++) {
        int v = pcm[i];
        if (v < 0) v = -v;
        sum_abs += v;
        if (v > peak) peak = v;
    }
    int avg = n ? (int)(sum_abs / n) : 0;
    ESP_LOGI(TAG, "PTT audio level: avg=%d peak=%d gain=%d%s", avg, peak, gain,
             peak < 30 ? " (SILENT)" : "");
}

static void capture_task(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    void *encoder = NULL;
    int frame_bytes = 960 * (int)sizeof(int16_t);
    int out_size = 2048;
    esp_opus_enc_config_t cfg = {
        .sample_rate = 16000,
        .channel = 1,
        .bits_per_sample = 16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = false,
        .enable_vbr = true,
    };
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &encoder) != ESP_AUDIO_ERR_OK || !encoder) {
        ESP_LOGW(TAG, "opus encoder open failed");
        if (s_capture_gen == gen) {
            set_state(WS_AI_ERROR, "opus encoder");
            s_listening = false;
            send_listen_stop();
        }
        vTaskDeleteWithCaps(NULL);
        return;
    }
    /* The codec API returns the input frame size in BYTES.  The earlier code
     * treated it as samples, feeding two 60 ms frames while only reserving
     * space for one encoded frame. */
    esp_opus_enc_get_frame_size(encoder, &frame_bytes, &out_size);
    if (frame_bytes < (int)sizeof(int16_t) || out_size <= 0) {
        ESP_LOGE(TAG, "invalid opus frame sizes: in=%d bytes out=%d bytes",
                 frame_bytes, out_size);
        esp_opus_enc_close(encoder);
        if (s_capture_gen == gen) {
            set_state(WS_AI_ERROR, "opus frame size");
            s_listening = false;
            send_listen_stop();
        }
        vTaskDeleteWithCaps(NULL);
        return;
    }
    int frame_samples = frame_bytes / (int)sizeof(int16_t);
    int16_t *pcm = malloc((size_t)frame_bytes);
    uint8_t *out = malloc((size_t)out_size);
    if (!pcm || !out) {
        free(pcm); free(out);
        esp_opus_enc_close(encoder);
        if (s_capture_gen == gen) {
            s_listening = false;
            send_listen_stop();
        }
        vTaskDeleteWithCaps(NULL);
        return;
    }
    ESP_LOGI(TAG, "PTT capture: %d samples / %d bytes per frame, out %d bytes",
             frame_samples, frame_bytes, out_size);
    int level_frames = 0;
    int warmup_frames = 3;
    while (s_listening && s_connected && !s_stop && s_capture_gen == gen) {
        if (watch_audio_read(pcm, frame_samples, 300) != ESP_OK) {
            continue;
        }
        if (warmup_frames > 0) {
            warmup_frames--;
            continue;
        }
        int gain = 1;
        condition_audio(pcm, frame_samples, &gain);
        if (++level_frames >= 8) {
            log_capture_level(pcm, frame_samples, gain);
            level_frames = 0;
        }
        esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)pcm,
                                        .len = (uint32_t)frame_bytes };
        esp_audio_enc_out_frame_t o = { .buffer = out, .len = (uint32_t)out_size };
        if (esp_opus_enc_process(encoder, &in, &o) == ESP_AUDIO_ERR_OK && o.encoded_bytes) {
            send_bin(out, (int)o.encoded_bytes);
        }
    }
    free(pcm);
    free(out);
    esp_opus_enc_close(encoder);
    if (!s_stop_sent && s_capture_gen == gen) {
        send_listen_stop();
    }
    if (s_capture_gen == gen) {
        s_listening = false;
    }
    vTaskDeleteWithCaps(NULL);
}

static void handle_json(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "bad JSON");
        return;
    }
    cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";
    if (!strcmp(t, "hello")) {
        cJSON *ap = cJSON_GetObjectItem(root, "audio_params");
        cJSON *sr = ap ? cJSON_GetObjectItem(ap, "sample_rate") : NULL;
        if (cJSON_IsNumber(sr) && sr->valueint >= 8000) {
            s_tts_rate = sr->valueint;
        }
        set_state(WS_AI_IDLE, "online");
    } else if (!strcmp(t, "stt")) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text) && text->valuestring[0]) {
            if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
            snprintf(s_user, sizeof(s_user), "%s", text->valuestring);
            snprintf(s_live_user, sizeof(s_live_user), "%s", text->valuestring);
            s_live_user_on = true;
            s_live_reply[0] = '\0';
            s_live_reply_on = false;
            s_reply[0] = '\0';
            if (s_info_lock) xSemaphoreGive(s_info_lock);
        }
        set_state(WS_AI_THINKING, "thinking");
    } else if (!strcmp(t, "llm")) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        cJSON *state = cJSON_GetObjectItem(root, "state");
        const char *st = cJSON_IsString(state) ? state->valuestring : "";
        bool has_text = cJSON_IsString(text) && text->valuestring[0];
        if (has_text) {
            if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
            snprintf(s_reply, sizeof(s_reply), "%s", text->valuestring);
            snprintf(s_live_reply, sizeof(s_live_reply), "%s", text->valuestring);
            s_live_reply_on = true;
            if (!s_live_user_on && s_user[0]) {
                snprintf(s_live_user, sizeof(s_live_user), "%s", s_user);
                s_live_user_on = true;
            }
            if (s_info_lock) xSemaphoreGive(s_info_lock);
        }
        if (!strcmp(st, "done") || !st[0]) {
            bool stored = false;
            if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
            if (s_live_user_on && s_live_reply_on) {
                stored = chat_commit_round_locked(s_live_user, s_live_reply);
                if (stored) {
                    s_live_user_on = false;
                    s_live_reply_on = false;
                    s_live_user[0] = '\0';
                    s_live_reply[0] = '\0';
                }
            }
            if (s_info_lock) xSemaphoreGive(s_info_lock);
            set_state(WS_AI_SPEAKING, "speaking");
        } else {
            set_state(WS_AI_THINKING, "thinking");
        }
    } else if (!strcmp(t, "tts")) {
        cJSON *state = cJSON_GetObjectItem(root, "state");
        const char *st = cJSON_IsString(state) ? state->valuestring : "";
        if (!strcmp(st, "stop")) {
            set_state(WS_AI_IDLE, "online");
        } else {
            set_state(WS_AI_SPEAKING, "speaking");
        }
    } else if (!strcmp(t, "pong")) {
        set_state(WS_AI_IDLE, "pong rx");
    } else if (!strcmp(t, "error")) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        set_state(WS_AI_ERROR, cJSON_IsString(msg) ? msg->valuestring : "server error");
    } else if (!strcmp(t, "mcp")) {
        /* MCP is optional in WatchShell; acknowledge unknown requests quietly. */
        ESP_LOGI(TAG, "MCP message ignored");
    }
    cJSON_Delete(root);
}

static int read_message(uint8_t **out, ws_transport_opcodes_t *op_out)
{
    *out = NULL;
    if (op_out) *op_out = WS_TRANSPORT_OPCODES_NONE;
    esp_transport_handle_t transport = s_transport;
    if (!transport) return -1;
    uint8_t tmp[2048];
    int got = esp_transport_read(transport, (char *)tmp, sizeof(tmp), 1000);
    if (got <= 0) return got;
    ws_transport_opcodes_t op = esp_transport_ws_get_read_opcode(transport);
    if (op_out) *op_out = op;
    int payload = esp_transport_ws_get_read_payload_len(transport);
    if (payload > got && payload < 64 * 1024) {
        uint8_t *buf = malloc((size_t)payload);
        if (!buf) return -1;
        memcpy(buf, tmp, (size_t)got);
        int done = got;
        while (done < payload) {
            int n = esp_transport_read(transport, (char *)buf + done, payload - done, 1000);
            if (n <= 0) break;
            done += n;
        }
        *out = buf;
        return done;
    }
    uint8_t *buf = malloc((size_t)got);
    if (!buf) return -1;
    memcpy(buf, tmp, (size_t)got);
    *out = buf;
    return got;
}

static esp_transport_handle_t open_transport(void)
{
    esp_transport_handle_t parent = s_secure ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (!parent) return NULL;
    if (s_secure) {
        esp_transport_ssl_crt_bundle_attach(parent, esp_crt_bundle_attach);
    }
    esp_transport_handle_t ws = esp_transport_ws_init(parent);
    if (!ws) {
        esp_transport_destroy(parent);
        return NULL;
    }
    static char headers[320];
    headers[0] = '\0';
    if (s_token[0]) {
        snprintf(headers, sizeof(headers),
                 "Authorization: Bearer %s\r\nProtocol-Version: 1\r\n", s_token);
    } else {
        snprintf(headers, sizeof(headers), "Protocol-Version: 1\r\n");
    }
    esp_transport_ws_config_t cfg = {
        .ws_path = s_path,
        .headers = headers,
        .propagate_control_frames = true,
    };
    esp_transport_ws_set_config(ws, &cfg);
    return ws;
}

static void ws_task(void *arg)
{
    (void)arg;
    while (!s_stop) {
        if (!watch_net_connected()) {
            set_state(WS_AI_OFFLINE, "no wifi");
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
        set_state(WS_AI_CONNECTING, "connecting");
        s_transport = open_transport();
        if (!s_transport) {
            set_state(WS_AI_ERROR, "transport");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        int ret = esp_transport_connect(s_transport, s_host, s_port, 10000);
        if (ret < 0) {
            ESP_LOGE(TAG, "connect %s:%d failed", s_host, s_port);
            close_transport();
            set_state(WS_AI_ERROR, "server unreachable");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        s_connected = true;
        const char *hello =
            "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
            "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
            "\"channels\":1,\"frame_duration\":60},"
            "\"features\":{\"mcp\":false,\"aec\":false}}";
        if (!send_text(hello)) {
            ESP_LOGW(TAG, "hello send failed");
            close_transport();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "websocket connected to %s%s", s_host, s_path);
        set_state(WS_AI_CONNECTING, "waiting hello");

        while (!s_stop && s_connected) {
            check_state_timeout();
            uint8_t *buf = NULL;
            ws_transport_opcodes_t op = WS_TRANSPORT_OPCODES_NONE;
            int len = read_message(&buf, &op);
            if (len < 0) {
                ESP_LOGW(TAG, "websocket read failed: %d", len);
                break;
            }
            if (len == 0) {
                free(buf);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (op == WS_TRANSPORT_OPCODES_TEXT) {
                handle_json((const char *)buf, len);
            } else if (op == WS_TRANSPORT_OPCODES_BINARY) {
                audio_enqueue(buf, len);
                buf = NULL;
            } else if (op == WS_TRANSPORT_OPCODES_PING) {
                if (s_send_lock) xSemaphoreTake(s_send_lock, portMAX_DELAY);
                esp_transport_handle_t transport = s_transport;
                if (transport) {
                    esp_transport_ws_send_raw(transport,
                                              WS_TRANSPORT_OPCODES_PONG | WS_TRANSPORT_OPCODES_FIN,
                                              (const char *)buf, len, 1000);
                }
                if (s_send_lock) xSemaphoreGive(s_send_lock);
            } else if (op == WS_TRANSPORT_OPCODES_CLOSE) {
                free(buf);
                break;
            }
            free(buf);
        }
        close_transport();
        s_listening = false;
        audio_queue_clear();
        set_state(WS_AI_OFFLINE, "disconnected");
        if (!s_stop) vTaskDelay(pdMS_TO_TICKS(2000));
    }
    s_ws_task = NULL;
    vTaskDelete(NULL);
}

void watch_cloud_init(void)
{
    if (!s_send_lock) s_send_lock = xSemaphoreCreateMutex();
    if (!s_info_lock) s_info_lock = xSemaphoreCreateMutex();
    if (!s_audio_q) s_audio_q = xQueueCreate(8, sizeof(audio_pkt_t));
    if (!s_play_task && s_audio_q) {
        if (xTaskCreatePinnedToCore(play_task, "ai_play", 40960, NULL, 6,
                                    &s_play_task, 1) != pdPASS) {
            ESP_LOGE(TAG, "ai_play task create failed");
            s_play_task = NULL;
        }
    }
    set_state(WS_AI_OFFLINE, "offline");
}

void watch_cloud_start(void)
{
    if (s_ws_task) return;
    watch_config_get_ai(s_url, sizeof(s_url), s_token, sizeof(s_token));
    if (!s_url[0]) {
        set_state(WS_AI_OFFLINE, "no ai url");
        return;
    }
    if (!parse_url(s_url)) {
        set_state(WS_AI_ERROR, "bad ai url");
        return;
    }
    s_stop = false;
    xTaskCreatePinnedToCore(ws_task, "ai_ws", 12288, NULL, 5, &s_ws_task, 0);
}

void watch_cloud_stop(void)
{
    s_stop = true;
    s_listening = false;
    close_transport();
    /* Wait for the old task to leave its outer loop before a new task can be
     * started.  Otherwise a slow read can be woken by a subsequent start(),
     * see s_stop=false and open a second WebSocket connection. */
    for (int i = 0; i < 30 && s_ws_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    audio_queue_clear();
    set_state(WS_AI_OFFLINE, "offline");
}

bool watch_cloud_connected(void) { return s_connected; }
ws_ai_state_t watch_cloud_state(void) { return s_state; }
bool watch_cloud_listening(void) { return s_listening; }

void watch_cloud_info(ws_ai_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->connected = s_connected;
    if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
    snprintf(out->user_text, sizeof(out->user_text), "%s", s_user);
    snprintf(out->reply_text, sizeof(out->reply_text), "%s", s_reply);
    snprintf(out->detail, sizeof(out->detail), "%s", s_detail);
    if (s_info_lock) xSemaphoreGive(s_info_lock);
}

int watch_cloud_chat_count(void)
{
    int count = 0;
    if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
    count = s_chat_count;
    if (s_info_lock) xSemaphoreGive(s_info_lock);
    return count;
}

bool watch_cloud_chat_get(int index, ws_ai_chat_msg_t *out)
{
    if (!out || index < 0) return false;
    bool ok = false;
    if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
    if (index < s_chat_count) {
        *out = s_chat[index];
        ok = true;
    }
    if (s_info_lock) xSemaphoreGive(s_info_lock);
    return ok;
}

void watch_cloud_chat_clear(void)
{
    if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
    s_chat_count = 0;
    s_live_user_on = false;
    s_live_reply_on = false;
    s_live_user[0] = '\0';
    s_live_reply[0] = '\0';
    s_user[0] = '\0';
    s_reply[0] = '\0';
    if (s_info_lock) xSemaphoreGive(s_info_lock);
}

bool watch_cloud_live_get(ws_ai_role_t role, char *out, int cap)
{
    if (!out || cap <= 0) return false;
    bool found = false;
    if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
    if (role == WS_AI_ROLE_USER ? s_live_user_on : s_live_reply_on) {
        const char *src = role == WS_AI_ROLE_USER ? s_live_user : s_live_reply;
        snprintf(out, (size_t)cap, "%s", src);
        found = out[0] != '\0';
    } else {
        out[0] = '\0';
    }
    if (s_info_lock) xSemaphoreGive(s_info_lock);
    return found;
}

bool watch_cloud_send_chat(const char *text)
{
    if (!text || !text[0]) return false;
    if (!s_connected) {
        set_state(WS_AI_ERROR, "offline");
        return false;
    }
    if (s_listening) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "type", "chat");
    cJSON_AddStringToObject(root, "text", text);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;

    bool ok = send_text(json);
    cJSON_free(json);
    if (!ok) {
        set_state(WS_AI_ERROR, "send failed");
        return false;
    }

    if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
    snprintf(s_user, sizeof(s_user), "%s", text);
    s_reply[0] = '\0';
    if (s_info_lock) xSemaphoreGive(s_info_lock);
    set_state(WS_AI_THINKING, "thinking");
    return true;
}

bool watch_cloud_send_ping(void)
{
    if (!s_connected) {
        set_state(WS_AI_ERROR, "offline");
        return false;
    }
    if (!send_text("{\"type\":\"ping\"}")) {
        set_state(WS_AI_ERROR, "ping failed");
        return false;
    }
    set_state(s_state, "ping sent");
    return true;
}

void watch_cloud_listen(bool on)
{
    if (!on) {
        if (s_listening) {
            s_listening = false;
            set_state(WS_AI_THINKING, "thinking");
            /* Send stop from the UI task immediately on release.  Previously
             * only capture_task sent it, so a failed/slow task creation left
             * the server waiting forever after a lone listen:start. */
            if (send_listen_stop()) {
                s_stop_sent = true;
            }
        }
        return;
    }
    if (!s_connected) {
        set_state(WS_AI_ERROR, "offline");
        return;
    }
    if (s_listening) return;
    if (!watch_audio_ready()) {
        if (watch_audio_init() != ESP_OK) {
            set_state(WS_AI_ERROR, "audio init");
            return;
        }
    }
    if (s_info_lock) xSemaphoreTake(s_info_lock, portMAX_DELAY);
    s_user[0] = '\0';
    s_reply[0] = '\0';
    if (s_info_lock) xSemaphoreGive(s_info_lock);
    s_listening = true;
    s_stop_sent = false;
    uint32_t gen = ++s_capture_gen;
    set_state(WS_AI_LISTENING, "listening");
    send_text("{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}");
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        capture_task, "ai_cap", 40960, (void *)(uintptr_t)gen, 6, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "ai_cap create failed: internal=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        s_listening = false;
        set_state(WS_AI_ERROR, "capture task");
        if (send_listen_stop()) {
            s_stop_sent = true;
        }
    } else {
        ESP_LOGI(TAG, "ai_cap started with PSRAM stack, psram free %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

const char *watch_cloud_state_name(ws_ai_state_t state)
{
    switch (state) {
    case WS_AI_CONNECTING: return "CONNECTING";
    case WS_AI_IDLE: return "ONLINE";
    case WS_AI_LISTENING: return "LISTENING";
    case WS_AI_THINKING: return "THINKING";
    case WS_AI_SPEAKING: return "SPEAKING";
    case WS_AI_ERROR: return "ERROR";
    default: return "OFFLINE";
    }
}
