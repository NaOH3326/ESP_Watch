#include "watch_term.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "watch_gfx.h"
#include "watch_types.h"

typedef struct {
    char ch[WS_TERM_COLS];
    uint16_t fg[WS_TERM_COLS];
    uint8_t len;
} term_line_t;

static term_line_t s_lines[WS_TERM_LINES];
static int s_head;          /* next line to write */
static int s_count;
static int s_scroll;        /* 0 = tail */
static char s_prompt[32] = "root@esp-watch:~#";
static SemaphoreHandle_t s_mutex;

static void lock(void) { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

void watch_term_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    watch_term_clear();
}

void watch_term_clear(void)
{
    lock();
    memset(s_lines, 0, sizeof(s_lines));
    s_head = 0;
    s_count = 0;
    s_scroll = 0;
    unlock();
}

static void new_line_locked(void)
{
    s_head = (s_head + 1) % WS_TERM_LINES;
    memset(&s_lines[s_head], 0, sizeof(s_lines[s_head]));
    if (s_count < WS_TERM_LINES) {
        s_count++;
    }
    s_scroll = 0;
}

static void put_char_locked(char c, uint16_t color)
{
    term_line_t *line = &s_lines[s_head];
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        new_line_locked();
        return;
    }
    if (line->len >= WS_TERM_COLS) {
        new_line_locked();
        line = &s_lines[s_head];
    }
    line->ch[line->len] = c;
    line->fg[line->len] = color;
    line->len++;
}

void watch_term_write(const char *text, uint16_t color)
{
    if (!text) return;
    lock();
    for (const char *p = text; *p; p++) {
        put_char_locked(*p, color);
    }
    unlock();
}

static bool tag_color(const char *p, uint16_t *color, int *consumed)
{
    if (p[0] != '{') return false;
    const char *end = strchr(p, '}');
    if (!end || end - p > 12) return false;
    char name[12];
    size_t n = (size_t)(end - p - 1);
    memcpy(name, p + 1, n);
    name[n] = '\0';
    if (!strcmp(name, "/")) {
        *color = WS_COLOR_WHITE;   /* caller replaces with base */
        *consumed = (int)(end - p + 1);
        return true;
    }
    uint16_t c = watch_gfx_color_name(name);
    if (c == WS_COLOR_GREEN && strcasecmp(name, "green")) {
        return false;
    }
    *color = c;
    *consumed = (int)(end - p + 1);
    return true;
}

void watch_term_write_tagged(const char *text, uint16_t base_color)
{
    if (!text) return;
    uint16_t color = base_color;
    lock();
    const char *p = text;
    while (*p) {
        uint16_t c;
        int used;
        if (tag_color(p, &c, &used)) {
            color = (c == WS_COLOR_WHITE && p[1] == '/') ? base_color : c;
            p += used;
            continue;
        }
        put_char_locked(*p++, color);
    }
    unlock();
}

void watch_term_printf(uint16_t color, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    watch_term_write_tagged(buf, color);
}

void watch_term_set_prompt(const char *prompt)
{
    lock();
    snprintf(s_prompt, sizeof(s_prompt), "%s", prompt ? prompt : "");
    unlock();
}

const char *watch_term_prompt(void) { return s_prompt; }

int watch_term_line_count(void)
{
    lock();
    int n = s_count;
    unlock();
    return n;
}

int watch_term_scroll(void)
{
    lock();
    int n = s_scroll;
    unlock();
    return n;
}

void watch_term_scroll_by(int delta)
{
    lock();
    s_scroll += delta;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > s_count) s_scroll = s_count;
    unlock();
}

void watch_term_set_scroll(int line)
{
    lock();
    s_scroll = line;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > s_count) s_scroll = s_count;
    unlock();
}

void watch_term_follow(void)
{
    lock();
    s_scroll = 0;
    unlock();
}

void watch_term_dump(void)
{
    lock();
    int total = s_count;
    printf("--- terminal (%d lines) ---\n", total);
    for (int logical = 0; logical < total; logical++) {
        int idx = (total < WS_TERM_LINES) ? logical : (s_head + 1 + logical) % WS_TERM_LINES;
        term_line_t *line = &s_lines[idx];
        fwrite(line->ch, 1, line->len, stdout);
        fputc('\n', stdout);
    }
    printf("--------------------------\n");
    unlock();
}

void watch_term_render(int x, int y, int max_lines, uint16_t default_color, bool cursor)
{
    lock();
    int total = s_count;
    int visible = max_lines < total ? max_lines : total;
    int end = total - s_scroll;          /* exclusive logical index */
    int start = end - visible;
    if (start < 0) start = 0;
    int line_h = watch_gfx_text_height(1);
    for (int i = 0; i < visible; i++) {
        int logical = start + i;
        int idx;
        if (total < WS_TERM_LINES) {
            idx = logical;
        } else {
            idx = (s_head + 1 + logical) % WS_TERM_LINES;
        }
        term_line_t *line = &s_lines[idx];
        int yy = y + i * line_h;
        for (int j = 0; j < line->len; j++) {
            watch_gfx_char(x + j * 6, yy, line->ch[j], line->fg[j], 1);
        }
    }
    if (cursor && s_scroll == 0) {
        int yy = y + visible * line_h;
        int xx = x;
        if (visible > 0) {
            int idx = (total < WS_TERM_LINES) ? (total - 1) : s_head;
            xx += s_lines[idx].len * 6;
        }
        watch_gfx_text(xx, yy, "_", default_color, 1);
    }
    unlock();
}
