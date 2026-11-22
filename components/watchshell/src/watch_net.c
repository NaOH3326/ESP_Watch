#include "watch_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ft_sensors.h"
#include "sdkconfig.h"
#include "watch_cloud.h"
#include "watch_config.h"
#include "watch_storage.h"
#include "watch_video.h"

static const char *TAG = "WATCH_NET";
static EventGroupHandle_t s_events;
static bool s_netif_ready;
static bool s_wifi_ready;
static bool s_connected;
static bool s_ap_mode;
static bool s_portal_started;
static char s_ip[16] = "0.0.0.0";
static char s_ssid[33];
static httpd_handle_t s_httpd;
static int s_retry;
static bool s_sntp_started;

static void sntp_sync_cb(struct timeval *tv)
{
    if (!tv) return;
    esp_err_t err = ft_rtc_set_epoch(tv->tv_sec);
    ESP_LOGI(TAG, "SNTP synced, RTC write %s", err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static void start_sntp(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = sntp_sync_cb;
    cfg.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com)");
    } else {
        ESP_LOGW(TAG, "SNTP init: %s", esp_err_to_name(err));
    }
}

#define NET_CONNECTED_BIT BIT0
#define NET_FAIL_BIT      BIT1

static void start_httpd(void);

static const char *html_page =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>WatchShell</title><style>"
    "body{background:#050807;color:#9dffb0;font:14px monospace;margin:20px}"
    "h1{color:#39ff88;font-size:20px}fieldset{border:1px solid #246b3d;margin:14px 0}"
    "input,button{background:#07110b;color:#d6ffe0;border:1px solid #39ff88;"
    "padding:8px;margin:4px;font:14px monospace}button{cursor:pointer}"
    "a{color:#39ff88}.muted{color:#5b8068}pre{white-space:pre-wrap}"
    "</style></head><body>"
    "<h1>WATCHSHELL // ESP32-S3</h1>"
    "<p class=muted>Configure Wi-Fi / cloud AI, upload .wsh scripts and media.</p>"
    "<fieldset><legend>NETWORK</legend><form id=cfg>"
    "SSID <input id=ssid name=ssid size=24><br>"
    "PASS <input id=pass name=pass type=password size=24><br>"
    "<button type=submit>SAVE + REBOOT</button></form></fieldset>"
    "<fieldset><legend>AI CLOUD</legend><form id=ai>"
    "URL <input id=url name=url size=42 placeholder='wss://host/path'><br>"
    "TOKEN <input id=token name=token size=42><br>"
    "<button type=submit>SAVE AI</button></form></fieldset>"
    "<fieldset><legend>VIDEO CATALOG</legend><form id=vid>"
    "URL <input id=video_url name=video_url size=52 "
    "placeholder='http://host:8080/watch/api/v1/videos'><br>"
    "TOKEN <input id=video_token name=video_token size=42><br>"
    "<button type=submit>SAVE VIDEO</button></form></fieldset>"
    "<fieldset><legend>MUSIC CATALOG</legend><form id=mus>"
    "URL <input id=music_url name=music_url size=52 "
    "placeholder='http://host:8080/watch/api/v1/music'><br>"
    "TOKEN <input id=music_token name=music_token size=42><br>"
    "<button type=submit>SAVE MUSIC</button></form></fieldset>"
    "<fieldset><legend>SCRIPT / MEDIA UPLOAD</legend>"
    "<input id=file type=file accept='.wsh,.sh,.mp3,.wav,.aac,.jpg,.jpeg'><button id=up>UPLOAD</button>"
    "<p class=muted>Scripts: .wsh/.sh &rarr; /storage/scripts &nbsp; "
    "Music: .mp3/.wav/.aac &rarr; /storage/music &nbsp; Images: .jpg/.jpeg &rarr; /storage/video "
    "(browser resizes to 440x276 JPEG)</p>"
    "</fieldset><pre id=out class=muted>ready.</pre>"
    "<script>"
    "async function send(u,b,ct){let r=await fetch(u,{method:'POST',body:b,headers:ct?{'Content-Type':ct}:{}});"
    "document.getElementById('out').textContent=await r.text();}"
    "async function resizeImage(f){"
    "if(!f.type||f.type.indexOf('image/')!==0)return f;"
    "try{let b=await createImageBitmap(f);"
    "let s=Math.min(1,440/b.width,276/b.height);"
    "let w=Math.max(1,Math.round(b.width*s)),h=Math.max(1,Math.round(b.height*s));"
    "let c=document.createElement('canvas');c.width=w;c.height=h;"
    "c.getContext('2d').drawImage(b,0,0,w,h);if(b.close)b.close();"
    "let z=await new Promise(r=>c.toBlob(r,'image/jpeg',0.82));"
    "if(!z)return f;let n=f.name;let d=n.lastIndexOf('.');if(d>0)n=n.slice(0,d);"
    "return new File([z],n+'.jpg',{type:'image/jpeg'});"
    "}catch(e){return f;}}"
    "document.getElementById('cfg').onsubmit=async e=>{e.preventDefault();"
    "let b=new URLSearchParams(new FormData(e.target)).toString();"
    "await send('/api/config',b,'application/x-www-form-urlencoded');};"
    "document.getElementById('ai').onsubmit=async e=>{e.preventDefault();"
    "let b=new URLSearchParams(new FormData(e.target)).toString();"
    "await send('/api/config',b,'application/x-www-form-urlencoded');};"
    "document.getElementById('vid').onsubmit=async e=>{e.preventDefault();"
    "let b=new URLSearchParams(new FormData(e.target)).toString();"
    "await send('/api/config',b,'application/x-www-form-urlencoded');};"
    "document.getElementById('mus').onsubmit=async e=>{e.preventDefault();"
    "let b=new URLSearchParams(new FormData(e.target)).toString();"
    "await send('/api/config',b,'application/x-www-form-urlencoded');};"
    "document.getElementById('up').onclick=async()=>{"
    "let f=document.getElementById('file').files[0];if(!f){return;}"
    "document.getElementById('out').textContent='preparing '+f.name+'...';"
    "f=await resizeImage(f);"
    "document.getElementById('out').textContent='uploading '+f.name+' ('+f.size+' bytes)...';"
    "try{await send('/api/upload?name='+encodeURIComponent(f.name),f,'application/octet-stream');}"
    "catch(e){document.getElementById('out').textContent='upload failed: '+e;}};"
    "</script></body></html>";


static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 1 < dst_len; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    if (dst_len) dst[o] = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t out_len)
{
    if (out && out_len) out[0] = '\0';
    if (!body || !key || !out || out_len < 2) return false;
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > key_len && !strncmp(p, key, key_len) && p[key_len] == '=') {
            char tmp[256];
            size_t n = seg - key_len - 1;
            if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
            memcpy(tmp, p + key_len + 1, n);
            tmp[n] = '\0';
            url_decode(tmp, out, out_len);
            return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"ip\":\"%s\",\"ap\":%s,\"connected\":%s,\"rssi\":%d,\"storage\":%s}",
             s_ip, s_ap_mode ? "true" : "false", s_connected ? "true" : "false",
             watch_net_rssi(), watch_storage_ready() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_post(httpd_req_t *req)
{
    char body[1024];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) return ESP_FAIL;
    body[got] = '\0';

    char ssid[64] = {0}, pass[64] = {0}, url[192] = {0}, token[192] = {0};
    char vurl[192] = {0}, vtoken[192] = {0};
    char murl[192] = {0}, mtoken[192] = {0};
    bool changed_wifi = false;
    if (form_get(body, "ssid", ssid, sizeof(ssid)) && ssid[0]) {
        form_get(body, "pass", pass, sizeof(pass));
        watch_config_set_wifi(ssid, pass);
        changed_wifi = true;
    }
    if (form_get(body, "url", url, sizeof(url)) && url[0]) {
        form_get(body, "token", token, sizeof(token));
        watch_config_set_ai(url, token);
        watch_cloud_stop();
        watch_cloud_start();
    }
    if (form_get(body, "video_url", vurl, sizeof(vurl))) {
        form_get(body, "video_token", vtoken, sizeof(vtoken));
        watch_config_set_video(vurl, vtoken);
        watch_video_stop_playback();
    }
    if (form_get(body, "music_url", murl, sizeof(murl))) {
        form_get(body, "music_token", mtoken, sizeof(mtoken));
        watch_config_set_music(murl, mtoken);
    }
    if (changed_wifi) {
        httpd_resp_send(req, "saved; rebooting into station mode...\n", HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(700));
        esp_restart();
        return ESP_OK;
    }
    httpd_resp_send(req, "saved.\n", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static bool safe_name(const char *name, char *out, size_t out_len)
{
    if (!name || !name[0]) return false;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    const char *bs = strrchr(base, '\\');
    if (bs) base = bs + 1;
    size_t o = 0;
    for (size_t i = 0; base[i] && o + 1 < out_len; i++) {
        char c = base[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return o > 0;
}

static bool has_suffix(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    return !strcasecmp(s + n - m, suffix);
}

static esp_err_t upload_post(httpd_req_t *req)
{
    char query[256] = {0};
    char raw_name[96] = {0};
    char name[96] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", raw_name, sizeof(raw_name)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
    }
    url_decode(raw_name, name, sizeof(name));
    char clean[96];
    if (!safe_name(name, clean, sizeof(clean))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    const char *dir = WS_SCRIPT_DIR;
    if (has_suffix(clean, ".mp3") || has_suffix(clean, ".wav") || has_suffix(clean, ".aac")) {
        dir = WS_MUSIC_DIR;
    } else if (has_suffix(clean, ".jpg") || has_suffix(clean, ".jpeg")) {
        dir = WS_VIDEO_DIR;
    } else if (!has_suffix(clean, ".wsh") && !has_suffix(clean, ".sh")) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "only .wsh/.sh/.mp3/.wav/.aac/.jpg/.jpeg");
    }
    int len = req->content_len;
    if (len <= 0 || len > 6 * 1024 * 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size (max 6 MB)");
    }
    if (!watch_storage_ready()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage not ready");
    }

    char path[160];
    char tmp[168];
    snprintf(path, sizeof(path), "%s/%s", dir, clean);
    snprintf(tmp, sizeof(tmp), "%s/.up_%s", dir, clean);

    FILE *f = fopen(tmp, "wb");
    if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "create failed");

    uint8_t *chunk = malloc(4096);
    if (!chunk) {
        fclose(f);
        remove(tmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }

    /* Stream the HTTP body to the FAT filesystem in small chunks.  Keeping the
     * whole upload in RAM caused UI/network stalls for multi-megabyte JPEGs. */
    int done = 0;
    int timeouts = 0;
    bool ok = true;
    while (done < len) {
        int want = len - done > 4096 ? 4096 : len - done;
        int got = httpd_req_recv(req, (char *)chunk, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                ok = false;
                break;
            }
            continue;
        }
        if (got <= 0 || fwrite(chunk, 1, (size_t)got, f) != (size_t)got) {
            ok = false;
            break;
        }
        done += got;
        timeouts = 0;
    }
    if (fclose(f) != 0) ok = false;
    free(chunk);

    if (!ok || done != len) {
        remove(tmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload interrupted");
    }

    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
    }

    char msg[240];
    snprintf(msg, sizeof(msg), "OK %s (%d bytes)\n", path, len);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static void start_httpd(void)
{
    if (s_portal_started) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "httpd start failed; retry on next network event");
        s_httpd = NULL;
        return;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get };
    httpd_uri_t config = { .uri = "/api/config", .method = HTTP_POST, .handler = config_post };
    httpd_uri_t upload = { .uri = "/api/upload", .method = HTTP_POST, .handler = upload_post };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &status);
    httpd_register_uri_handler(s_httpd, &config);
    httpd_register_uri_handler(s_httpd, &upload);
    s_portal_started = true;
    ESP_LOGI(TAG, "portal at http://%s/", s_ip);
}

static void wifi_start_sta(void)
{
    char ssid[33], pass[64];
    watch_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    if (!ssid[0]) {
        ESP_LOGW(TAG, "no Wi-Fi credentials");
        return;
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(s_ap_mode ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA config: %s", esp_err_to_name(err));
        return;
    }
    esp_wifi_connect();
}

static void wifi_start_ap(void)
{
    if (s_ap_mode) {
        start_httpd();
        return;
    }
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, CONFIG_WATCH_AP_SSID, sizeof(cfg.ap.ssid) - 1);
    strncpy((char *)cfg.ap.password, CONFIG_WATCH_AP_PASSWORD, sizeof(cfg.ap.password) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen((char *)cfg.ap.ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 3;
    cfg.ap.authmode = strlen(CONFIG_WATCH_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP config: %s", esp_err_to_name(err));
        return;
    }
    s_ap_mode = true;
    snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
    /* HTTP is started from WIFI_EVENT_AP_START, after the AP netif is up. */
    ESP_LOGI(TAG, "AP '%s' password '%s'", CONFIG_WATCH_AP_SSID, CONFIG_WATCH_AP_PASSWORD);
}

static void net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_retry++;
        xEventGroupClearBits(s_events, NET_CONNECTED_BIT);
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA disconnected reason=%d (retry %d)",
                 disc ? disc->reason : -1, s_retry);
        if (s_retry >= 3 && !s_ap_mode) {
            ESP_LOGW(TAG, "STA failed, starting config AP");
            wifi_start_ap();
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        xEventGroupSetBits(s_events, NET_CONNECTED_BIT);
        start_httpd();
        start_sntp();
        ESP_LOGI(TAG, "connected, IP %s", s_ip);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        s_ap_mode = true;
        snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
        start_httpd();
    }
}

esp_err_t watch_net_init(void)
{
    if (!s_events) {
        s_events = xEventGroupCreate();
    }
    if (!s_netif_ready) {
        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
            return err;
        }
        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();
        s_netif_ready = true;
    }
    if (!s_wifi_ready) {
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, net_event, NULL));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, net_event, NULL));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_wifi_set_ps(WIFI_PS_NONE);   /* lower latency for AI audio */
        s_wifi_ready = true;
    }
    if (watch_config_has_wifi()) {
        wifi_start_sta();
    } else {
        wifi_start_ap();
    }
    return ESP_OK;
}

bool watch_net_connected(void) { return s_connected; }
bool watch_net_ap_mode(void) { return s_ap_mode; }
const char *watch_net_ip(void) { return s_ip; }

int watch_net_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return -127;
}

void watch_net_status(ws_net_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->connected = s_connected;
    out->ap_mode = s_ap_mode;
    out->rssi = watch_net_rssi();
    snprintf(out->ip, sizeof(out->ip), "%s", s_ip);
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_ssid);
}

esp_err_t watch_net_start_portal(void)
{
    wifi_start_ap();
    return ESP_OK;
}

esp_err_t watch_net_restart_sta(void)
{
    s_retry = 0;
    if (watch_config_has_wifi()) {
        esp_wifi_disconnect();
        wifi_start_sta();
    } else {
        wifi_start_ap();
    }
    return ESP_OK;
}
