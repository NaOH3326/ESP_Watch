#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Persisted chat history: each message is small enough to copy onto the UI
 * task stack, while the total budget bounds the history to roughly eight
 * short rounds.  A round that does not fit is still shown live, but is not
 * added to this persistent ring. */
#define WS_AI_TEXT_MAX 1024
#define WS_AI_LIVE_TEXT_MAX 8192
#define WS_AI_CHAT_ROUNDS 8
#define WS_AI_CHAT_MAX (WS_AI_CHAT_ROUNDS * 2)
#define WS_AI_CHAT_BUDGET 8192

typedef enum {
    WS_AI_OFFLINE = 0,
    WS_AI_CONNECTING,
    WS_AI_IDLE,
    WS_AI_LISTENING,
    WS_AI_THINKING,
    WS_AI_SPEAKING,
    WS_AI_ERROR,
} ws_ai_state_t;

typedef enum {
    WS_AI_ROLE_USER = 0,
    WS_AI_ROLE_ASSISTANT,
} ws_ai_role_t;

typedef struct {
    uint8_t role;                 /* ws_ai_role_t */
    char text[WS_AI_TEXT_MAX];
} ws_ai_chat_msg_t;

typedef struct {
    ws_ai_state_t state;
    bool connected;
    char user_text[WS_AI_TEXT_MAX];
    char reply_text[WS_AI_TEXT_MAX];
    char detail[64];
} ws_ai_info_t;

void watch_cloud_init(void);
void watch_cloud_start(void);
void watch_cloud_stop(void);

bool watch_cloud_connected(void);
ws_ai_state_t watch_cloud_state(void);
void watch_cloud_info(ws_ai_info_t *out);

/* Push-to-talk.  Calling listen(false) flushes the last frame and sends stop. */
void watch_cloud_listen(bool on);
bool watch_cloud_listening(void);

/* Chat history exposed to the AI app.  Messages are kept in the cloud layer,
 * so they survive page switches and WebSocket reconnects.  Only complete
 * rounds that fit the storage budget are committed here. */
int watch_cloud_chat_count(void);
bool watch_cloud_chat_get(int index, ws_ai_chat_msg_t *out);
void watch_cloud_chat_clear(void);

/* Current round display.  The latest user/assistant text is exposed even when
 * it is too large for the persistent history; the UI draws it live. */
bool watch_cloud_live_get(ws_ai_role_t role, char *out, int cap);


/* Text-path test helpers used by the AI presets.
 * chat sends {"type":"chat","text":...}; ping is a raw WS round trip that
 * needs neither ASR nor an AI provider. */
bool watch_cloud_send_chat(const char *text);
bool watch_cloud_send_ping(void);

const char *watch_cloud_state_name(ws_ai_state_t state);
