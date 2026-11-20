#include "watch_apps.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "watch_cloud.h"
#include "watch_config.h"
#include "watch_gfx.h"
#include "watch_net.h"

#define AI_HOME_BTN_X  30
#define AI_HOME_BTN_Y  194
#define AI_HOME_BTN_W  180
#define AI_HOME_BTN_H  34
#define AI_CHAT_TOP    48
#define AI_CHAT_BOTTOM 244
#define AI_PTT_Y       250
#define AI_PTT_H       22
#define AI_HOLD_MS     220
#define AI_GAP         5
#define AI_PAD         5
#define AI_LABEL_H     8
#define AI_LINE_H      16
#define AI_MAX_TEXT_W  196
#define AI_LINE_BUF    192

typedef enum {
    AI_PAGE_HOME = 0,
    AI_PAGE_CHAT,
} ai_page_t;

static ai_page_t s_page;
static bool s_home_pressed;
static bool s_back_pressed;
static bool s_clear_pressed;
static bool s_ptt_pending;
static bool s_holding;
static int64_t s_ptt_down_ms;
static bool s_chat_drag;
static int s_chat_down_y;
static int s_chat_drag_scroll;
static int s_chat_scroll;
static char s_live_user[WS_AI_LIVE_TEXT_MAX];
static char s_live_reply[WS_AI_LIVE_TEXT_MAX];

static bool ai_hit_home_button(int x, int y)
{
    return x >= AI_HOME_BTN_X && x < AI_HOME_BTN_X + AI_HOME_BTN_W &&
           y >= AI_HOME_BTN_Y && y < AI_HOME_BTN_Y + AI_HOME_BTN_H;
}

static bool ai_hit_back(int x, int y)
{
    return x >= WS_SAFE && x < 62 && y >= WS_UI_TOP && y < 46;
}

static bool ai_hit_clear(int x, int y)
{
    return x >= 188 && x < WS_SCREEN_W - WS_SAFE && y >= WS_UI_TOP && y < 46;
}

static bool ai_hit_ptt(int x, int y)
{
    return x >= WS_SAFE && x < WS_SCREEN_W - WS_SAFE &&
           y >= AI_PTT_Y && y < AI_PTT_Y + AI_PTT_H;
}

static uint32_t ai_next_cp(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    if (!p || !*p) return 0;
    uint32_t cp;
    int extra;
    if (*p < 0x80) {
        cp = *p;
        extra = 0;
    } else if ((*p & 0xE0) == 0xC0) {
        cp = *p & 0x1F;
        extra = 1;
    } else if ((*p & 0xF0) == 0xE0) {
        cp = *p & 0x0F;
        extra = 2;
    } else if ((*p & 0xF8) == 0xF0) {
        cp = *p & 0x07;
        extra = 3;
    } else {
        (*text)++;
        return '?';
    }
    p++;
    for (int i = 0; i < extra; i++) {
        if ((*p & 0xC0) != 0x80) {
            *text = (const char *)p;
            return '?';
        }
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    *text = (const char *)p;
    return cp;
}

static int ai_cp_width(uint32_t cp)
{
    return cp < 0x80 ? 6 : 14;
}

typedef struct {
    int lines;
    int width;
} ai_metrics_t;

static ai_metrics_t ai_measure(const char *text, int max_width)
{
    ai_metrics_t out = { 1, 0 };
    int line_w = 0;
    const char *p = text ? text : "";
    while (*p) {
        uint32_t cp = ai_next_cp(&p);
        if (cp == '\r') continue;
        if (cp == '\n') {
            if (line_w > out.width) out.width = line_w;
            line_w = 0;
            out.lines++;
            continue;
        }
        int w = ai_cp_width(cp);
        if (line_w > 0 && line_w + w > max_width) {
            if (line_w > out.width) out.width = line_w;
            out.lines++;
            line_w = 0;
        }
        line_w += w;
    }
    if (line_w > out.width) out.width = line_w;
    return out;
}

static int ai_message_height(const char *text)
{
    ai_metrics_t m = ai_measure(text, AI_MAX_TEXT_W);
    return AI_PAD + AI_LABEL_H + 2 + m.lines * AI_LINE_H + 3;
}

static void ai_flush_line(const char *line, int len, int x, int y,
                          int clip_top, int clip_bottom, uint16_t color)
{
    if (len <= 0) return;
    if (y < clip_top || y + 14 > clip_bottom) return;
    char buf[AI_LINE_BUF];
    int copy = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, line, (size_t)copy);
    buf[copy] = '\0';
    watch_gfx_cjk_text(x, y, buf, color);
}

/* Draw wrapped 14 px text, clipping lines to the transcript area. */
static int ai_draw_wrapped(const char *text, int x, int y0, int max_width,
                           int clip_top, int clip_bottom, uint16_t color)
{
    char line[AI_LINE_BUF];
    int len = 0;
    int line_w = 0;
    int y = y0;
    const char *p = text ? text : "";
    while (*p) {
        const char *start = p;
        uint32_t cp = ai_next_cp(&p);
        if (cp == '\r') continue;
        if (cp == '\n') {
            ai_flush_line(line, len, x, y, clip_top, clip_bottom, color);
            y += AI_LINE_H;
            len = 0;
            line_w = 0;
            continue;
        }
        int w = ai_cp_width(cp);
        if (line_w > 0 && line_w + w > max_width) {
            ai_flush_line(line, len, x, y, clip_top, clip_bottom, color);
            y += AI_LINE_H;
            len = 0;
            line_w = 0;
        }
        int n = (int)(p - start);
        if (len + n < (int)sizeof(line)) {
            memcpy(line + len, start, (size_t)n);
            len += n;
            line_w += w;
        }
    }
    ai_flush_line(line, len, x, y, clip_top, clip_bottom, color);
    return y - y0 + 14;
}

static int ai_draw_message(uint8_t role, const char *text, int top,
                           int clip_top, int clip_bottom)
{
    const char *body = text ? text : "";
    ai_metrics_t m = ai_measure(body, AI_MAX_TEXT_W);
    int bubble_w = m.width + 2 * AI_PAD;
    if (bubble_w < 70) bubble_w = 70;
    if (bubble_w > 216) bubble_w = 216;
    int bubble_h = AI_PAD + AI_LABEL_H + 2 + m.lines * AI_LINE_H + 3;
    int x = role == WS_AI_ROLE_USER ? WS_SCREEN_W - WS_SAFE - bubble_w : WS_SAFE;
    int bottom = top + bubble_h;
    int vis_top = top > clip_top ? top : clip_top;
    int vis_bottom = bottom < clip_bottom ? bottom : clip_bottom;

    if (vis_bottom > vis_top) {
        uint16_t border = role == WS_AI_ROLE_USER ? WS_COLOR_CYAN : WS_COLOR_GREEN;
        uint16_t fill = role == WS_AI_ROLE_USER ? 0x0140 : 0x0104;
        watch_gfx_fill_rect(x, vis_top, bubble_w, vis_bottom - vis_top, fill);
        if (top >= clip_top && top < clip_bottom) {
            watch_gfx_hline(x, top, bubble_w, border);
        }
        if (bottom - 1 >= clip_top && bottom - 1 < clip_bottom) {
            watch_gfx_hline(x, bottom - 1, bubble_w, border);
        }
        watch_gfx_vline(x, vis_top, vis_bottom - vis_top, border);
        watch_gfx_vline(x + bubble_w - 1, vis_top, vis_bottom - vis_top, border);

        if (top + 4 >= clip_top && top + 12 < clip_bottom) {
            watch_gfx_text(x + AI_PAD, top + 4,
                           role == WS_AI_ROLE_USER ? "YOU" : "AI",
                           role == WS_AI_ROLE_USER ? WS_COLOR_CYAN : WS_COLOR_GREEN, 1);
        }
        ai_draw_wrapped(body, x + AI_PAD, top + AI_PAD + AI_LABEL_H + 2,
                        bubble_w - 2 * AI_PAD, clip_top, clip_bottom,
                        role == WS_AI_ROLE_USER ? WS_COLOR_WHITE : WS_COLOR_GREEN);
    }
    return bubble_h;
}

static void ai_draw_chat_log(int clip_bottom)
{
    int count = watch_cloud_chat_count();
    bool live_user = watch_cloud_live_get(WS_AI_ROLE_USER, s_live_user,
                                          sizeof(s_live_user));
    bool live_reply = watch_cloud_live_get(WS_AI_ROLE_ASSISTANT, s_live_reply,
                                           sizeof(s_live_reply));
    int items = count + (live_user ? 1 : 0) + (live_reply ? 1 : 0);
    int total = 0;
    ws_ai_chat_msg_t msg;
    for (int i = 0; i < count; i++) {
        if (!watch_cloud_chat_get(i, &msg)) continue;
        total += ai_message_height(msg.text) + AI_GAP;
    }
    if (live_reply) total += ai_message_height(s_live_reply) + AI_GAP;
    if (live_user) total += ai_message_height(s_live_user) + AI_GAP;
    if (items > 0) total -= AI_GAP;

    int available = clip_bottom - AI_CHAT_TOP;
    int max_scroll = total > available ? total - available : 0;
    if (s_chat_scroll > max_scroll) s_chat_scroll = max_scroll;
    if (s_chat_scroll < 0) s_chat_scroll = 0;

    if (items == 0) {
        const char *hint = "按住底部按钮开始对话";
        int w = watch_gfx_cjk_text_width(hint);
        watch_gfx_cjk_text((WS_SCREEN_W - w) / 2, 112, hint, WS_COLOR_DIM);
        watch_gfx_text_center(120, 136, "ASR + DeepSeek", WS_COLOR_DIM, 1);
        return;
    }

    int bottom = clip_bottom + s_chat_scroll;
    if (live_reply) {
        int h = ai_message_height(s_live_reply);
        int top = bottom - h;
        if (top < clip_bottom && bottom > AI_CHAT_TOP) {
            ai_draw_message(WS_AI_ROLE_ASSISTANT, s_live_reply, top,
                            AI_CHAT_TOP, clip_bottom);
        }
        bottom = top - AI_GAP;
    }
    if (live_user) {
        int h = ai_message_height(s_live_user);
        int top = bottom - h;
        if (top < clip_bottom && bottom > AI_CHAT_TOP) {
            ai_draw_message(WS_AI_ROLE_USER, s_live_user, top,
                            AI_CHAT_TOP, clip_bottom);
        }
        bottom = top - AI_GAP;
    }
    for (int i = count - 1; i >= 0; i--) {
        if (!watch_cloud_chat_get(i, &msg)) continue;
        int h = ai_message_height(msg.text);
        int top = bottom - h;
        if (top < clip_bottom && bottom > AI_CHAT_TOP) {
            ai_draw_message(msg.role, msg.text, top, AI_CHAT_TOP, clip_bottom);
        }
        bottom = top - AI_GAP;
        if (bottom < AI_CHAT_TOP) break;
    }
}

static void ai_render_home(void)
{
    ws_ai_info_t info;
    watch_cloud_info(&info);
    bool configured = watch_config_has_ai();
    int pulse = (int)((esp_timer_get_time() / 180000) % 10);
    uint16_t state_color = info.state == WS_AI_ERROR ? WS_COLOR_RED :
                           info.state == WS_AI_SPEAKING ? WS_COLOR_AMBER :
                           info.connected ? WS_COLOR_GREEN : WS_COLOR_GRAY;

    watch_gfx_scanlines(WS_UI_TOP, 150, 0x0120);
    watch_gfx_text(WS_SAFE, 26, "AI / VOICE", WS_COLOR_CYAN, 1);
    watch_gfx_text_right(WS_SCREEN_W - WS_SAFE, 26,
                         info.connected ? "ONLINE" : "OFFLINE",
                         info.connected ? WS_COLOR_GREEN : WS_COLOR_GRAY, 1);

    watch_gfx_circle(120, 100, 48, WS_COLOR_DIM);
    watch_gfx_circle(120, 100, 38, state_color);
    watch_gfx_arc(120, 100, 54 + (s_holding ? pulse : 0), 200, 340,
                  info.state == WS_AI_LISTENING ? WS_COLOR_CYAN : WS_COLOR_DIM);
    watch_gfx_arc(120, 100, 61 - (s_holding ? pulse / 2 : 0), 205, 335, 0x01A0);
    watch_gfx_fill_circle(120, 100, 10, state_color);

    watch_gfx_text_center(120, 158, watch_cloud_state_name(info.state),
                          info.state == WS_AI_ERROR ? WS_COLOR_RED : WS_COLOR_WHITE, 2);
    watch_gfx_text_center(120, 177, info.detail, WS_COLOR_GRAY, 1);

    bool pressed = s_home_pressed;
    watch_gfx_fill_rect(AI_HOME_BTN_X, AI_HOME_BTN_Y, AI_HOME_BTN_W, AI_HOME_BTN_H,
                        pressed ? 0x02A0 : 0x0104);
    watch_gfx_rect(AI_HOME_BTN_X, AI_HOME_BTN_Y, AI_HOME_BTN_W, AI_HOME_BTN_H,
                   pressed ? WS_COLOR_WHITE : WS_COLOR_GREEN);
    watch_gfx_text_center(120, AI_HOME_BTN_Y + 10, "OPEN CHAT",
                          pressed ? WS_COLOR_WHITE : WS_COLOR_GREEN, 2);

    if (!configured) {
        watch_gfx_text_center(120, 236, "SET AI URL IN PORTAL", WS_COLOR_AMBER, 1);
        watch_gfx_text_center(120, 250, watch_net_ip(), WS_COLOR_CYAN, 1);
    } else {
        watch_gfx_text_center(120, 238, "VOICE CHAT / DEEPSEEK", WS_COLOR_DIM, 1);
        watch_gfx_text_center(120, 252, "TAP OPEN CHAT TO START", WS_COLOR_DIM, 1);
    }
}

static void ai_render_chat(void)
{
    ws_ai_info_t info;
    watch_cloud_info(&info);
    int count = watch_cloud_chat_count();
    ws_ai_chat_msg_t last;
    bool have_last = count > 0 && watch_cloud_chat_get(count - 1, &last);
    bool live_user = watch_cloud_live_get(WS_AI_ROLE_USER, s_live_user,
                                          sizeof(s_live_user));
    bool live_reply = watch_cloud_live_get(WS_AI_ROLE_ASSISTANT, s_live_reply,
                                           sizeof(s_live_reply));
    bool typing = info.state == WS_AI_THINKING && !s_holding && !live_reply &&
                  (live_user || !have_last || last.role == WS_AI_ROLE_USER);

    watch_gfx_fill_rect(0, WS_UI_TOP, WS_SCREEN_W, 24, WS_COLOR_BLACK);
    watch_gfx_text(WS_SAFE, 28, "< BACK", WS_COLOR_CYAN, 1);
    watch_gfx_text_center(120, 28, "CHAT", WS_COLOR_WHITE, 1);
    watch_gfx_text_right(WS_SCREEN_W - WS_SAFE, 28, "CLR", WS_COLOR_DIM, 1);
    watch_gfx_text(WS_SAFE, 38, watch_cloud_state_name(info.state),
                   info.state == WS_AI_ERROR ? WS_COLOR_RED : WS_COLOR_GREEN, 1);
    watch_gfx_hline(WS_SAFE, 46, WS_SCREEN_W - 2 * WS_SAFE, WS_COLOR_DIM);

    int log_bottom = AI_CHAT_BOTTOM;
    int typing_h = ai_message_height("正在思考...");
    if (typing) log_bottom = AI_CHAT_BOTTOM - typing_h - AI_GAP;
    ai_draw_chat_log(log_bottom);
    if (typing) {
        ai_draw_message(WS_AI_ROLE_ASSISTANT, "正在思考...",
                        AI_CHAT_BOTTOM - typing_h, AI_CHAT_TOP, AI_CHAT_BOTTOM);
    }

    const char *label;
    if (!info.connected) label = "CLOUD OFFLINE";
    else if (s_holding) label = "RELEASE TO SEND";
    else if (s_ptt_pending) label = "KEEP HOLDING...";
    else label = "HOLD TO TALK";
    uint16_t ptt_color = s_holding ? WS_COLOR_GREEN :
                         s_ptt_pending ? WS_COLOR_AMBER : WS_COLOR_DIM;
    watch_gfx_fill_rect(WS_SAFE, AI_PTT_Y, WS_SCREEN_W - 2 * WS_SAFE, AI_PTT_H,
                        s_holding ? 0x02A0 : WS_COLOR_BLACK);
    watch_gfx_rect(WS_SAFE, AI_PTT_Y, WS_SCREEN_W - 2 * WS_SAFE, AI_PTT_H, ptt_color);
    watch_gfx_text_center(120, AI_PTT_Y + 7, label,
                          s_holding ? WS_COLOR_WHITE : ptt_color, 1);
}


static void ai_stop_ptt(void)
{
    bool was_holding = s_holding;
    s_ptt_pending = false;
    s_holding = false;
    if (was_holding) watch_cloud_listen(false);
}

static void ai_enter(void)
{
    s_page = AI_PAGE_HOME;
    s_home_pressed = false;
    s_back_pressed = false;
    s_clear_pressed = false;
    s_ptt_pending = false;
    s_holding = false;
    s_chat_drag = false;
    s_chat_scroll = 0;
    watch_cloud_start();
}

static void ai_leave(void)
{
    ai_stop_ptt();
    s_page = AI_PAGE_HOME;
    s_home_pressed = false;
    s_back_pressed = false;
    s_clear_pressed = false;
    s_chat_drag = false;
}

static void ai_tick(void)
{
    if (s_page != AI_PAGE_CHAT) return;
    if (!watch_cloud_connected()) {
        if (!s_holding) s_ptt_pending = false;
        return;
    }
    if (s_ptt_pending && !s_holding) {
        int64_t elapsed = esp_timer_get_time() - s_ptt_down_ms;
        if (elapsed >= (int64_t)AI_HOLD_MS * 1000) {
            s_ptt_pending = false;
            s_holding = true;
            watch_cloud_listen(true);
            if (!watch_cloud_listening()) s_holding = false;
        }
    }
    if (s_holding && !watch_cloud_listening()) s_holding = false;
}

static void ai_home_event(const ws_event_t *ev)
{
    if (ev->type == WS_EV_TOUCH_DOWN) {
        s_home_pressed = ai_hit_home_button(ev->x, ev->y);
    } else if (ev->type == WS_EV_TOUCH_UP) {
        bool hit = ai_hit_home_button(ev->x, ev->y);
        bool pressed = s_home_pressed;
        s_home_pressed = false;
        if (pressed && hit) {
            s_page = AI_PAGE_CHAT;
            s_chat_scroll = 0;
        }
    }
}

static void ai_chat_event(const ws_event_t *ev)
{
    if (ev->type == WS_EV_TOUCH_DOWN) {
        if (ai_hit_back(ev->x, ev->y)) {
            s_back_pressed = true;
            return;
        }
        if (ai_hit_clear(ev->x, ev->y)) {
            s_clear_pressed = true;
            return;
        }
        if (ai_hit_ptt(ev->x, ev->y)) {
            /* Do not start capture on a stray tap.  A deliberate hold arms
             * PTT in ai_tick() after AI_HOLD_MS; this prevents random empty
             * ASR requests from touches in the chat area. */
            s_ptt_pending = true;
            s_ptt_down_ms = esp_timer_get_time();
            return;
        }
        if (ev->y >= AI_CHAT_TOP && ev->y < AI_PTT_Y) {
            s_chat_drag = true;
            s_chat_down_y = ev->y;
            s_chat_drag_scroll = s_chat_scroll;
        }
    } else if (ev->type == WS_EV_TOUCH_MOVE) {
        if (s_back_pressed && !ai_hit_back(ev->x, ev->y)) s_back_pressed = false;
        if (s_clear_pressed && !ai_hit_clear(ev->x, ev->y)) s_clear_pressed = false;
        if (s_ptt_pending && !s_holding && !ai_hit_ptt(ev->x, ev->y)) {
            s_ptt_pending = false;
        }
        if (s_chat_drag) {
            /* The chat pane uses the opposite drag sign from the old build:
             * the transcript now follows the finger's up/down direction. */
            int next = s_chat_drag_scroll + (ev->y - s_chat_down_y);
            s_chat_scroll = next > 0 ? next : 0;
        }
    } else if (ev->type == WS_EV_TOUCH_UP) {
        if (s_back_pressed) {
            bool hit = ai_hit_back(ev->x, ev->y);
            s_back_pressed = false;
            if (hit) {
                ai_stop_ptt();
                s_page = AI_PAGE_HOME;
                s_chat_drag = false;
            }
            return;
        }
        if (s_clear_pressed) {
            bool hit = ai_hit_clear(ev->x, ev->y);
            s_clear_pressed = false;
            if (hit) {
                watch_cloud_chat_clear();
                s_chat_scroll = 0;
            }
            return;
        }
        s_chat_drag = false;
        if (s_ptt_pending || s_holding) {
            ai_stop_ptt();
        }
    }
}

static void ai_event(const ws_event_t *ev)
{
    if (!ev) return;
    if (s_page == AI_PAGE_HOME) ai_home_event(ev);
    else ai_chat_event(ev);
}

static void ai_render(void)
{
    if (s_page == AI_PAGE_HOME) ai_render_home();
    else ai_render_chat();
}

const ws_app_impl_t ws_app_ai = {
    .id = "ai",
    .name = "AI",
    .subtitle = "voice chat / deepseek",
    .enter = ai_enter,
    .leave = ai_leave,
    .render = ai_render,
    .event = ai_event,
    .tick = ai_tick,
};
