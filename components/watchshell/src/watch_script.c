#include "watch_script.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "ft_pmic.h"
#include "watch_audio.h"
#include "watch_config.h"
#include "watch_gfx.h"
#include "watch_net.h"
#include "watch_power.h"
#include "watch_sensors.h"
#include "watch_storage.h"
#include "watch_term.h"
#include "watch_ui.h"

#define MAX_LINES 512
#define MAX_ARGS 16
#define MAX_VARS 12

static const char *TAG = "WATCH_SCRIPT";

static volatile bool s_busy;
static volatile bool s_stop;
static char s_current[48];
static TaskHandle_t s_task;

typedef struct {
    char name[12];
    char value[40];
} var_t;

typedef struct {
    char *lines[MAX_LINES];
    int count;
    char *labels[MAX_LINES];
    int label_line[MAX_LINES];
    int label_count;
    var_t vars[MAX_VARS];
    int var_count;
    uint16_t echo_color;
} script_ctx_t;

static void print_help(void)
{
    watch_term_printf(WS_COLOR_CYAN, "COMMANDS\n");
    watch_term_printf(WS_COLOR_GREEN,
        "  echo [color] <text>   print (inline {red}tags{/} work)\n"
        "  set NAME VALUE        variable\n"
        "  sleep MS              delay\n"
        "  label NAME / goto NAME\n"
        "  if A OP B goto NAME   op: == != > < >= <=\n"
        "  clear / prompt TEXT\n"
        "  help / info / free / date / batt\n"
        "  sensors / wifi scan / wifi status\n"
        "  tone FREQ MS / volume 0-100\n"
        "  gpio set|read|toggle PIN [0|1]\n"
        "  app list / app open NAME\n"
        "  matrix [MS] / reboot / poweroff\n");
}

static void print_info(void)
{
    ws_power_t p = watch_sensors_power();
    watch_term_printf(WS_COLOR_GREEN, "chip    ESP32-S3 %s\n", esp_get_idf_version());
    watch_term_printf(WS_COLOR_GREEN, "ip      %s  wifi=%s\n",
                      watch_net_ip(), watch_net_connected() ? "up" : "down");
    watch_term_printf(WS_COLOR_GREEN, "battery %d%%  %d mV  vsys %d mV\n",
                      p.batt_pct, p.batt_mv, p.vsys_mv);
    watch_term_printf(WS_COLOR_GREEN, "heap    %u bytes  psram %u bytes\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    watch_term_printf(WS_COLOR_GREEN, "uptime  %lld s\n", esp_timer_get_time() / 1000000);
    watch_term_printf(WS_COLOR_GREEN, "script  %s\n", s_current[0] ? s_current : "-");
}

static void print_sensors(void)
{
    const ws_imu_t *imu = watch_sensors_imu();
    const ws_mag_t *mag = watch_sensors_mag();
    const ws_env_t *env = watch_sensors_env();
    if (imu->ok) {
        watch_term_printf(WS_COLOR_CYAN, "IMU  a %.2f %.2f %.2f g\n", imu->ax, imu->ay, imu->az);
        watch_term_printf(WS_COLOR_CYAN, "     g %.1f %.1f %.1f dps  %.1fC\n",
                          imu->gx, imu->gy, imu->gz, imu->temp_c);
        watch_term_printf(WS_COLOR_CYAN, "     steps %d\n", imu->steps);
    } else {
        watch_term_printf(WS_COLOR_RED, "IMU  offline\n");
    }
    if (mag->ok) {
        watch_term_printf(WS_COLOR_CYAN, "MAG  %d %d %d  heading %.1f deg\n",
                          mag->x, mag->y, mag->z, mag->heading_deg);
    } else {
        watch_term_printf(WS_COLOR_RED, "MAG  offline\n");
    }
    if (env->ok) {
        watch_term_printf(WS_COLOR_CYAN, "ENV  %.1f C  %.1f %%RH\n", env->temp_c, env->rh);
    } else {
        watch_term_printf(WS_COLOR_RED, "ENV  offline\n");
    }
}

static int tokenize(char *line, char *argv[], int max_args)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        char quote = 0;
        if (*p == '"' || *p == '\'') {
            quote = *p++;
        }
        argv[argc++] = p;
        if (quote) {
            char *end = strchr(p, quote);
            if (end) {
                *end = '\0';
                p = end + 1;
            } else {
                p += strlen(p);
            }
        } else {
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

static const char *var_get(script_ctx_t *ctx, const char *name)
{
    for (int i = 0; i < ctx->var_count; i++) {
        if (!strcmp(ctx->vars[i].name, name)) return ctx->vars[i].value;
    }
    return "";
}

static void expand_vars(script_ctx_t *ctx, const char *in, char *out, size_t out_len);

static bool var_exists(script_ctx_t *ctx, const char *name)
{
    for (int i = 0; i < ctx->var_count; i++) {
        if (!strcmp(ctx->vars[i].name, name)) return true;
    }
    return false;
}

static void arg_value(script_ctx_t *ctx, const char *in, char *out, size_t out_len)
{
    if (var_exists(ctx, in)) {
        expand_vars(ctx, var_get(ctx, in), out, out_len);
    } else {
        expand_vars(ctx, in, out, out_len);
    }
}

static void var_set(script_ctx_t *ctx, const char *name, const char *value)
{
    for (int i = 0; i < ctx->var_count; i++) {
        if (!strcmp(ctx->vars[i].name, name)) {
            snprintf(ctx->vars[i].value, sizeof(ctx->vars[i].value), "%s", value);
            return;
        }
    }
    if (ctx->var_count < MAX_VARS) {
        snprintf(ctx->vars[ctx->var_count].name, sizeof(ctx->vars[ctx->var_count].name), "%s", name);
        snprintf(ctx->vars[ctx->var_count].value, sizeof(ctx->vars[ctx->var_count].value), "%s", value);
        ctx->var_count++;
    }
}

static void expand_vars(script_ctx_t *ctx, const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    const char *p = in;
    while (*p && o + 1 < out_len) {
        if (p[0] == '$' && p[1] == '{') {
            const char *end = strchr(p + 2, '}');
            if (end) {
                char name[16];
                size_t n = (size_t)(end - p - 2);
                if (n >= sizeof(name)) n = sizeof(name) - 1;
                memcpy(name, p + 2, n);
                name[n] = '\0';
                const char *v = var_get(ctx, name);
                while (*v && o + 1 < out_len) out[o++] = *v++;
                p = end + 1;
                continue;
            }
        }
        if (p[0] == '$' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            char name[16];
            size_t n = 0;
            const char *q = p + 1;
            while ((isalnum((unsigned char)*q) || *q == '_') && n + 1 < sizeof(name)) {
                name[n++] = *q++;
            }
            name[n] = '\0';
            const char *v = var_get(ctx, name);
            while (*v && o + 1 < out_len) out[o++] = *v++;
            p = q;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

static void eval_numeric(const char *expr, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", expr ? expr : "");
    if (!expr) return;
    const char *p = expr;
    while (*p == ' ') p++;
    char *end = NULL;
    long a = strtol(p, &end, 10);
    if (end == p) return;
    while (*end == ' ') end++;
    char op = *end;
    if (op != '+' && op != '-' && op != '*' && op != '/') return;
    char *end2 = NULL;
    long b = strtol(end + 1, &end2, 10);
    if (end2 == end + 1) return;
    long v = 0;
    if (op == '+') v = a + b;
    else if (op == '-') v = a - b;
    else if (op == '*') v = a * b;
    else if (b != 0) v = a / b;
    else return;
    snprintf(out, out_len, "%ld", v);
}

static int find_label(script_ctx_t *ctx, const char *name)
{
    for (int i = 0; i < ctx->label_count; i++) {
        if (!strcmp(ctx->labels[i], name)) return ctx->label_line[i];
    }
    return -1;
}

static bool is_color_name(const char *s)
{
    return !strcasecmp(s, "black") || !strcasecmp(s, "white") ||
           !strcasecmp(s, "green") || !strcasecmp(s, "dim") ||
           !strcasecmp(s, "gray") || !strcasecmp(s, "grey") ||
           !strcasecmp(s, "cyan") || !strcasecmp(s, "amber") ||
           !strcasecmp(s, "yellow") || !strcasecmp(s, "red") ||
           !strcasecmp(s, "blue") || !strcasecmp(s, "magenta") ||
           !strcasecmp(s, "purple");
}

static void echo_cmd(script_ctx_t *ctx, int argc, char *argv[])
{
    if (argc < 2) {
        watch_term_write("\n", ctx->echo_color);
        return;
    }
    uint16_t color = ctx->echo_color;
    int first = 1;
    int last = argc;
    if (argc >= 3 && is_color_name(argv[1])) {
        color = watch_gfx_color_name(argv[1]);
        first = 2;
    } else if (argc >= 3 && is_color_name(argv[argc - 1])) {
        color = watch_gfx_color_name(argv[argc - 1]);
        last = argc - 1;
    }
    char text[256] = {0};
    size_t o = 0;
    for (int i = first; i < last && o + 1 < sizeof(text); i++) {
        char ex[160];
        expand_vars(ctx, argv[i], ex, sizeof(ex));
        if (i > first && o + 1 < sizeof(text)) text[o++] = ' ';
        for (const char *q = ex; *q && o + 1 < sizeof(text); q++) text[o++] = *q;
    }
    text[o] = '\0';
    watch_term_write_tagged(text, color);
    watch_term_write("\n", color);
}

static bool compare_values(const char *a, const char *op, const char *b)
{
    double na = atof(a), nb = atof(b);
    bool numeric = true;
    for (const char *p = a; *p; p++) if (!isdigit((unsigned char)*p) && *p != '-' && *p != '.') numeric = false;
    for (const char *p = b; *p; p++) if (!isdigit((unsigned char)*p) && *p != '-' && *p != '.') numeric = false;
    int cmp = numeric ? (na < nb ? -1 : na > nb ? 1 : 0) : strcmp(a, b);
    if (!strcmp(op, "==")) return cmp == 0;
    if (!strcmp(op, "!=")) return cmp != 0;
    if (!strcmp(op, ">")) return cmp > 0;
    if (!strcmp(op, "<")) return cmp < 0;
    if (!strcmp(op, ">=")) return cmp >= 0;
    if (!strcmp(op, "<=")) return cmp <= 0;
    return false;
}

static void cmd_gpio(script_ctx_t *ctx, int argc, char *argv[])
{
    (void)ctx;
    if (argc < 3) {
        watch_term_printf(WS_COLOR_RED, "usage: gpio set|read|toggle PIN [0|1]\n");
        return;
    }
    int pin = atoi(argv[2]);
    if (pin < 0 || pin > 48) {
        watch_term_printf(WS_COLOR_RED, "bad pin\n");
        return;
    }
    if (!strcmp(argv[1], "read")) {
        gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE };
        gpio_config(&cfg);
        watch_term_printf(WS_COLOR_GREEN, "gpio%d=%d\n", pin, gpio_get_level((gpio_num_t)pin));
        return;
    }
    gpio_config_t cfg = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT_OUTPUT,
                          .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                          .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&cfg);
    if (!strcmp(argv[1], "toggle")) {
        int v = gpio_get_level((gpio_num_t)pin) ? 0 : 1;
        gpio_set_level((gpio_num_t)pin, v);
        watch_term_printf(WS_COLOR_GREEN, "gpio%d=%d\n", pin, v);
    } else if (!strcmp(argv[1], "set") && argc >= 4) {
        int v = atoi(argv[3]) ? 1 : 0;
        gpio_set_level((gpio_num_t)pin, v);
        watch_term_printf(WS_COLOR_GREEN, "gpio%d=%d\n", pin, v);
    } else {
        watch_term_printf(WS_COLOR_RED, "usage: gpio set|read|toggle PIN [0|1]\n");
    }
}

static void cmd_wifi(int argc, char *argv[])
{
    if (argc >= 2 && !strcmp(argv[1], "status")) {
        ws_net_status_t st;
        watch_net_status(&st);
        watch_term_printf(WS_COLOR_GREEN, "wifi %s  ip %s  rssi %d  ap %s\n",
                          st.connected ? "connected" : "down", st.ip, st.rssi,
                          st.ap_mode ? "on" : "off");
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "scan")) {
        watch_term_printf(WS_COLOR_RED, "usage: wifi scan|status\n");
        return;
    }
    wifi_scan_config_t cfg = {0};
    watch_term_printf(WS_COLOR_CYAN, "scanning...\n");
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        watch_term_printf(WS_COLOR_RED, "scan failed: %s\n", esp_err_to_name(err));
        return;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 24) num = 24;
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (!recs) return;
    esp_wifi_scan_get_ap_records(&num, recs);
    for (int i = 0; i < num; i++) {
        watch_term_printf(WS_COLOR_GREEN, "%3d dBm  %-24s %s\n", recs[i].rssi,
                          (const char *)recs[i].ssid,
                          recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "wpa");
    }
    watch_term_printf(WS_COLOR_DIM, "%u networks\n", num);
    free(recs);
}

static void cmd_matrix(void)
{
    static const uint16_t colors[] = {WS_COLOR_GREEN, WS_COLOR_CYAN, WS_COLOR_DIM};
    for (int line = 0; line < 18; line++) {
        char buf[WS_TERM_COLS + 1];
        for (int i = 0; i < WS_TERM_COLS; i++) {
            int r = rand() % 7;
            buf[i] = (r == 0) ? (char)('0' + rand() % 10)
                              : (char)('!' + rand() % 90);
        }
        buf[WS_TERM_COLS] = '\0';
        watch_term_write(buf, colors[line % 3]);
        watch_term_write("\n", colors[line % 3]);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static int exec_line(script_ctx_t *ctx, int argc, char *argv[])
{
    if (argc == 0) return -1;
    const char *cmd = argv[0];
    if (!strcmp(cmd, "echo")) {
        echo_cmd(ctx, argc, argv);
    } else if (!strcmp(cmd, "set") && argc >= 3) {
        char ex[160], value[160];
        arg_value(ctx, argv[2], ex, sizeof(ex));
        eval_numeric(ex, value, sizeof(value));
        var_set(ctx, argv[1], value);
    } else if (!strcmp(cmd, "sleep") && argc >= 2) {
        int ms = atoi(argv[1]);
        if (ms < 0) ms = 0;
        if (ms > 60000) ms = 60000;
        for (int t = 0; t < ms && !s_stop; t += 50) {
            vTaskDelay(pdMS_TO_TICKS(ms - t > 50 ? 50 : ms - t));
        }
    } else if (!strcmp(cmd, "label")) {
        return -1;
    } else if (!strcmp(cmd, "goto") && argc >= 2) {
        int target = find_label(ctx, argv[1]);
        if (target < 0) watch_term_printf(WS_COLOR_RED, "no label: %s\n", argv[1]);
        return target;
    } else if (!strcmp(cmd, "if")) {
        if (argc >= 6 && !strcmp(argv[4], "goto")) {
            char a[80], b[80];
            arg_value(ctx, argv[1], a, sizeof(a));
            arg_value(ctx, argv[3], b, sizeof(b));
            if (compare_values(a, argv[2], b)) return find_label(ctx, argv[5]);
        } else {
            watch_term_printf(WS_COLOR_RED, "usage: if A OP B goto LABEL\n");
        }
    } else if (!strcmp(cmd, "clear")) {
        watch_term_clear();
    } else if (!strcmp(cmd, "prompt") && argc >= 2) {
        char text[64] = {0};
        for (int i = 1; i < argc; i++) {
            if (i > 1) strncat(text, " ", sizeof(text) - strlen(text) - 1);
            strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
        }
        watch_term_set_prompt(text);
    } else if (!strcmp(cmd, "help")) {
        print_help();
    } else if (!strcmp(cmd, "info")) {
        print_info();
    } else if (!strcmp(cmd, "free")) {
        watch_term_printf(WS_COLOR_GREEN, "heap %u  psram %u\n",
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else if (!strcmp(cmd, "batt")) {
        ws_power_t p = watch_sensors_power();
        watch_term_printf(WS_COLOR_GREEN, "battery %d%%  %d mV  vsys %d mV  vbus %d mV\n",
                          p.batt_pct, p.batt_mv, p.vsys_mv, p.vbus_mv);
    } else if (!strcmp(cmd, "date")) {
        const ws_time_t *t = watch_sensors_time();
        if (t->valid)
            watch_term_printf(WS_COLOR_GREEN, "%04d-%02d-%02d %02d:%02d:%02d\n",
                              t->year, t->month, t->day, t->hour, t->minute, t->second);
        else
            watch_term_printf(WS_COLOR_RED, "RTC not ready\n");
    } else if (!strcmp(cmd, "sensors")) {
        print_sensors();
    } else if (!strcmp(cmd, "wifi")) {
        cmd_wifi(argc, argv);
    } else if (!strcmp(cmd, "tone") && argc >= 3) {
        esp_err_t err = watch_audio_init();
        if (err != ESP_OK) {
            watch_term_printf(WS_COLOR_RED, "tone: audio init failed: %s\n",
                              esp_err_to_name(err));
        } else {
            err = watch_audio_tone(atoi(argv[1]), atoi(argv[2]));
            if (err != ESP_OK) {
                watch_term_printf(WS_COLOR_RED, "tone write failed: %s\n",
                                  esp_err_to_name(err));
            }
        }
    } else if (!strcmp(cmd, "volume") && argc >= 2) {
        int v = atoi(argv[1]);
        watch_audio_set_volume(v);
        watch_config_set_int("volume", v);
        watch_term_printf(WS_COLOR_GREEN, "volume %d%%\n", watch_audio_volume());
    } else if (!strcmp(cmd, "gpio")) {
        cmd_gpio(ctx, argc, argv);
    } else if (!strcmp(cmd, "app")) {
        if (argc >= 3 && !strcmp(argv[1], "open")) {
            if (!watch_ui_open_app_by_name(argv[2]))
                watch_term_printf(WS_COLOR_RED, "unknown app\n");
        } else {
            watch_term_printf(WS_COLOR_GREEN, "apps: watchface ai shell sensors media games settings\n");
        }
    } else if (!strcmp(cmd, "matrix")) {
        cmd_matrix();
    } else if (!strcmp(cmd, "reboot")) {
        watch_term_printf(WS_COLOR_AMBER, "rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (!strcmp(cmd, "poweroff")) {
        watch_term_printf(WS_COLOR_AMBER, "power off\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        watch_power_off(WS_POWEROFF_ACTIVE);
    } else {
        watch_term_printf(WS_COLOR_RED, "unknown: %s\n", cmd);
    }
    return -1;
}

static void add_label(script_ctx_t *ctx, const char *line, int index)
{
    char *copy = strdup(line);
    if (!copy) return;
    char *argv[MAX_ARGS];
    int argc = tokenize(copy, argv, MAX_ARGS);
    if (argc >= 2 && !strcmp(argv[0], "label") && ctx->label_count < MAX_LINES) {
        ctx->labels[ctx->label_count] = strdup(argv[1]);
        ctx->label_line[ctx->label_count] = index;
        ctx->label_count++;
    }
    free(copy);
}

static void run_script(char *buf, size_t len)
{
    script_ctx_t *ctx = heap_caps_calloc(1, sizeof(*ctx),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx) ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        watch_term_printf(WS_COLOR_RED, "script: no memory for context\n");
        return;
    }
    ctx->echo_color = WS_COLOR_GREEN;

    char *p = buf;
    char *end = buf + len;
    while (p < end && ctx->count < MAX_LINES) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (nl) *nl = '\0';
        char *line = p;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[n - 1] = '\0';
        ctx->lines[ctx->count++] = line;
        if (!nl) break;
        p = nl + 1;
    }
    for (int i = 0; i < ctx->count; i++) add_label(ctx, ctx->lines[i], i);

    int pc = 0;
    int guard = 0;
    int64_t start = esp_timer_get_time();
    while (pc < ctx->count && !s_stop) {
        if (++guard > 200000) {
            watch_term_printf(WS_COLOR_RED, "script guard: too many iterations\n");
            break;
        }
        if (esp_timer_get_time() - start > 120LL * 1000000) {
            watch_term_printf(WS_COLOR_RED, "script timeout (120s)\n");
            break;
        }
        char *copy = strdup(ctx->lines[pc]);
        if (!copy) break;
        char *argv[MAX_ARGS];
        int argc = tokenize(copy, argv, MAX_ARGS);
        if (argc > 0 && !strcmp(argv[0], "label")) {
            free(copy);
            pc++;
            continue;
        }
        int target = exec_line(ctx, argc, argv);
        free(copy);
        if (target >= 0) {
            pc = target;
        } else {
            pc++;
        }
        vTaskDelay(1);
    }
    if (s_stop) {
        watch_term_printf(WS_COLOR_AMBER, "^C stopped\n");
    }
    for (int i = 0; i < ctx->label_count; i++) free(ctx->labels[i]);
    free(ctx);
}

static void script_task(void *arg)
{
    char *path = (char *)arg;
    char *buf = NULL;
    size_t len = 0;
    watch_term_printf(WS_COLOR_DIM, "\n[exec] %s\n", s_current);
    esp_err_t err = watch_storage_read_all(path, &buf, &len);
    if (err != ESP_OK) {
        watch_term_printf(WS_COLOR_RED, "open failed: %s (%s)\n", path, watch_storage_error());
    } else {
        run_script(buf, len);
        free(buf);
    }
    free(path);
    s_busy = false;
    s_current[0] = '\0';
    s_task = NULL;
    vTaskDelete(NULL);
}

void watch_script_init(void)
{
    s_busy = false;
    s_stop = false;
}

bool watch_script_execute(const char *path)
{
    if (s_busy || !path) return false;
    char *copy = strdup(path);
    if (!copy) return false;
    s_busy = true;
    s_stop = false;
    const char *base = strrchr(path, '/');
    snprintf(s_current, sizeof(s_current), "%s", base ? base + 1 : path);
    /* This task reads FATFS/flash, so its stack must stay in internal RAM.
     * ESP-IDF disables the flash cache during the read and asserts if the
     * current task stack lives in PSRAM.  The script context is heap-allocated
     * in run_script(), so 16 KB is enough after video playback. */
    BaseType_t ok = xTaskCreatePinnedToCore(script_task, "wsh", 16384, copy, 4, &s_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "wsh task create failed (internal=%u psram=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        free(copy);
        s_busy = false;
        s_current[0] = '\0';
        return false;
    }
    return true;
}

void watch_script_stop(void)
{
    s_stop = true;
}

bool watch_script_busy(void) { return s_busy; }
const char *watch_script_current(void) { return s_current; }
