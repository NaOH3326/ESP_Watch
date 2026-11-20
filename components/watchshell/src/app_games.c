#include "watch_apps.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "watch_config.h"
#include "watch_gfx.h"
#include "watch_sensors.h"
#include "watch_ui.h"

#define GAME_MENU 0
#define GAME_SNAKE 1
#define GAME_2048 2
#define GAME_OVER 3
#define GAME_MATRIX 4
#define GAME_HEX 5
#define GAME_FLASK 6
#define GAMES_MENU_COUNT 5
#define GAMES_MENU_Y0 72
#define GAMES_MENU_STEP 36
#define GAMES_MENU_H 32
/* Measured on this watch: upright with gravity down gives ax negative;
 * rotating 90 degrees CCW puts gravity on ay.  Screen X = +sensor Y,
 * screen Y = -sensor X. */

#define RAIN_COLS 40
#define RAIN_ROWS 35
#define RAIN_CELL_W 6
#define RAIN_CELL_H 8

static int s_mode;
static int s_menu_sel;

static uint8_t s_rain_chars[RAIN_COLS][RAIN_ROWS];
static float s_rain_head[RAIN_COLS];
static float s_rain_speed[RAIN_COLS];
static uint8_t s_rain_len[RAIN_COLS];
static int64_t s_rain_last;

#define SNAKE_W 20
#define SNAKE_H 16
#define SNAKE_MAX 160
static int s_snake[SNAKE_MAX][2];
static int s_snake_len;
static int s_snake_dir;
static int s_food[2];
static int s_snake_score;
static int64_t s_last_move;
static bool s_snake_dead;

static int s_grid[4][4];
static int s_2048_score;
static bool s_2048_dead;

static void snake_reset(void)
{
    s_snake_len = 5;
    s_snake_dir = 1;
    s_snake_score = 0;
    s_snake_dead = false;
    for (int i = 0; i < s_snake_len; i++) {
        s_snake[i][0] = 5 - i;
        s_snake[i][1] = 8;
    }
    s_food[0] = 14;
    s_food[1] = 8;
    s_last_move = esp_timer_get_time();
}

static void place_food(void)
{
    for (int tries = 0; tries < 200; tries++) {
        int x = rand() % SNAKE_W;
        int y = rand() % SNAKE_H;
        bool used = false;
        for (int i = 0; i < s_snake_len; i++) {
            if (s_snake[i][0] == x && s_snake[i][1] == y) used = true;
        }
        if (!used) {
            s_food[0] = x;
            s_food[1] = y;
            return;
        }
    }
}

static void snake_step(void)
{
    if (s_snake_dead) return;
    int head[2] = { s_snake[0][0], s_snake[0][1] };
    if (s_snake_dir == 0) head[1]--;
    else if (s_snake_dir == 1) head[0]++;
    else if (s_snake_dir == 2) head[1]++;
    else head[0]--;
    if (head[0] < 0 || head[0] >= SNAKE_W || head[1] < 0 || head[1] >= SNAKE_H) {
        s_snake_dead = true;
        int hi = watch_config_get_int("snake_hi", 0);
        if (s_snake_score > hi) watch_config_set_int("snake_hi", s_snake_score);
        return;
    }
    for (int i = 0; i < s_snake_len - 1; i++) {
        if (s_snake[i][0] == head[0] && s_snake[i][1] == head[1]) {
            s_snake_dead = true;
            return;
        }
    }
    bool eat = head[0] == s_food[0] && head[1] == s_food[1];
    if (!eat) {
        for (int i = s_snake_len - 1; i > 0; i--) {
            s_snake[i][0] = s_snake[i - 1][0];
            s_snake[i][1] = s_snake[i - 1][1];
        }
    } else {
        if (s_snake_len < SNAKE_MAX) {
            for (int i = s_snake_len; i > 0; i--) {
                s_snake[i][0] = s_snake[i - 1][0];
                s_snake[i][1] = s_snake[i - 1][1];
            }
            s_snake_len++;
        }
        s_snake_score += 10;
        place_food();
    }
    s_snake[0][0] = head[0];
    s_snake[0][1] = head[1];
}

static void snake_draw(void)
{
    const int ox = WS_SAFE, oy = 42, cell = 11;
    watch_gfx_text(WS_SAFE, 26, "SNAKE", WS_COLOR_CYAN, 1);
    watch_gfx_printf(WS_SCREEN_W - WS_SAFE - 24, 26, WS_COLOR_GREEN, 1, "%d", s_snake_score);
    watch_gfx_rect(ox, oy, SNAKE_W * cell, SNAKE_H * cell, WS_COLOR_DIM);
    for (int y = 0; y < SNAKE_H; y++) {
        for (int x = 0; x < SNAKE_W; x++) {
            bool body = false;
            for (int i = 0; i < s_snake_len; i++) {
                if (s_snake[i][0] == x && s_snake[i][1] == y) body = true;
            }
            if (body) {
                watch_gfx_fill_rect(ox + x * cell + 1, oy + y * cell + 1, cell - 2, cell - 2,
                                    (x == s_snake[0][0] && y == s_snake[0][1]) ? WS_COLOR_WHITE : WS_COLOR_GREEN);
            }
        }
    }
    watch_gfx_fill_rect(ox + s_food[0] * cell + 3, oy + s_food[1] * cell + 3, cell - 6, cell - 6, WS_COLOR_RED);
    if (s_snake_dead) {
        watch_gfx_fill_rect(30, 118, 180, 48, WS_COLOR_BLACK);
        watch_gfx_rect(30, 118, 180, 48, WS_COLOR_RED);
        watch_gfx_text_center(120, 128, "GAME OVER", WS_COLOR_RED, 2);
        watch_gfx_text_center(120, 148, "TAP TO RETURN", WS_COLOR_GRAY, 1);
    }
}

static void grid_spawn(void)
{
    int empty[16][2];
    int n = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (!s_grid[y][x]) {
                empty[n][0] = x;
                empty[n][1] = y;
                n++;
            }
        }
    }
    if (!n) return;
    int i = rand() % n;
    s_grid[empty[i][1]][empty[i][0]] = (rand() % 10) ? 2 : 4;
}

static void grid_reset(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    s_2048_score = 0;
    s_2048_dead = false;
    grid_spawn();
    grid_spawn();
}

static bool grid_move_line(int *line, int *score)
{
    int out[4] = {0};
    int oi = 0;
    for (int i = 0; i < 4; i++) {
        if (line[i]) out[oi++] = line[i];
    }
    for (int i = 0; i < 3; i++) {
        if (out[i] && out[i] == out[i + 1]) {
            out[i] *= 2;
            *score += out[i];
            out[i + 1] = 0;
            i++;
        }
    }
    int final[4] = {0};
    oi = 0;
    for (int i = 0; i < 4; i++) if (out[i]) final[oi++] = out[i];
    bool changed = false;
    for (int i = 0; i < 4; i++) {
        if (line[i] != final[i]) changed = true;
        line[i] = final[i];
    }
    return changed;
}

static void grid_move(int dir)
{
    /* dir: 0 left, 1 up, 2 right, 3 down */
    bool changed = false;
    int score = 0;
    for (int row = 0; row < 4; row++) {
        int line[4];
        for (int i = 0; i < 4; i++) {
            int x = dir == 0 ? i : dir == 2 ? 3 - i : row;
            int y = dir == 1 ? i : dir == 3 ? 3 - i : row;
            if (dir == 0 || dir == 2) y = row;
            if (dir == 1 || dir == 3) x = row;
            line[i] = s_grid[y][x];
        }
        if (grid_move_line(line, &score)) changed = true;
        for (int i = 0; i < 4; i++) {
            int x = dir == 0 ? i : dir == 2 ? 3 - i : row;
            int y = dir == 1 ? i : dir == 3 ? 3 - i : row;
            if (dir == 0 || dir == 2) y = row;
            if (dir == 1 || dir == 3) x = row;
            s_grid[y][x] = line[i];
        }
    }
    if (changed) {
        s_2048_score += score;
        grid_spawn();
        bool any = false;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (!s_grid[y][x]) any = true;
        if (!any) {
            s_2048_dead = true;
            int hi = watch_config_get_int("2048_hi", 0);
            if (s_2048_score > hi) watch_config_set_int("2048_hi", s_2048_score);
        }
    }
}

static uint16_t tile_color(int v)
{
    switch (v) {
    case 2: return 0x0320;
    case 4: return 0x05E0;
    case 8: return 0x07E0;
    case 16: return 0x07FF;
    case 32: return 0xFD20;
    case 64: return 0xF800;
    case 128: return 0xF81F;
    default: return WS_COLOR_WHITE;
    }
}

static void game2048_draw(void)
{
    watch_gfx_text(WS_SAFE, 26, "2048", WS_COLOR_CYAN, 1);
    watch_gfx_printf(WS_SCREEN_W - WS_SAFE - 30, 26, WS_COLOR_GREEN, 1, "%d", s_2048_score);
    int ox = 24, oy = 46, cell = 48;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int px = ox + x * cell, py = oy + y * cell;
            watch_gfx_rect(px, py, cell - 2, cell - 2, WS_COLOR_DIM);
            int v = s_grid[y][x];
            if (v) {
                watch_gfx_fill_rect(px + 2, py + 2, cell - 6, cell - 6, 0x0200);
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", v);
                int scale = v >= 1000 ? 1 : v >= 100 ? 1 : 2;
                watch_gfx_text_center(px + cell / 2 - 1, py + cell / 2 - (scale == 2 ? 7 : 3),
                                      buf, tile_color(v), scale);
            }
        }
    }
    if (s_2048_dead) {
        watch_gfx_fill_rect(30, 118, 180, 48, WS_COLOR_BLACK);
        watch_gfx_rect(30, 118, 180, 48, WS_COLOR_RED);
        watch_gfx_text_center(120, 128, "GAME OVER", WS_COLOR_RED, 2);
        watch_gfx_text_center(120, 148, "TAP TO RETURN", WS_COLOR_GRAY, 1);
    }
}

static char rain_glyph(void)
{
    return (char)('!' + rand() % 94);
}

static void rain_column(int col, bool initial)
{
    s_rain_len[col] = (uint8_t)(10 + rand() % 20);
    s_rain_speed[col] = (float)(9 + rand() % 19);
    s_rain_head[col] = initial ? (float)(rand() % RAIN_ROWS)
                               : (float)(-(rand() % 24));
    for (int row = 0; row < RAIN_ROWS; row++) {
        s_rain_chars[col][row] = (uint8_t)rain_glyph();
    }
}

static void rain_reset(void)
{
    for (int c = 0; c < RAIN_COLS; c++) {
        rain_column(c, true);
    }
    s_rain_last = 0;
}

static void rain_step(void)
{
    int64_t now = esp_timer_get_time();
    if (!s_rain_last) {
        s_rain_last = now;
        return;
    }
    float dt = (float)(now - s_rain_last) / 1000000.0f;
    s_rain_last = now;
    if (dt > 0.1f) dt = 0.1f;

    for (int c = 0; c < RAIN_COLS; c++) {
        int old = (int)s_rain_head[c];
        s_rain_head[c] += s_rain_speed[c] * dt;
        int head = (int)s_rain_head[c];
        for (int r = old + 1; r <= head; r++) {
            int row = (r % RAIN_ROWS + RAIN_ROWS) % RAIN_ROWS;
            s_rain_chars[c][row] = (uint8_t)rain_glyph();
        }
        if (s_rain_head[c] - s_rain_len[c] >= RAIN_ROWS) {
            rain_column(c, false);
        }
    }
}

static void rain_draw(void)
{
    watch_gfx_clear(WS_COLOR_BLACK);
    for (int c = 0; c < RAIN_COLS; c++) {
        int head = (int)s_rain_head[c];
        int len = s_rain_len[c];
        for (int k = 0; k < len; k++) {
            int row = head - k;
            if (row < 0 || row >= RAIN_ROWS) continue;
            uint16_t color;
            if (k == 0) color = WS_COLOR_WHITE;
            else if (k < 4) color = WS_COLOR_GREEN;
            else if (k < 10) color = 0x03E0;
            else color = 0x01A0;
            watch_gfx_char(c * RAIN_CELL_W, row * RAIN_CELL_H,
                           (char)s_rain_chars[c][row], color, 1);
        }
    }
}

#define HEX_G_MAX          10
#define HEX_LEVEL_SAVE     "hex_g"
#define HEX_RESTITUTION    0.55f
#define HEX_GRAVITY_MPS2   9.80665f
#define HEX_PX_PER_M_X     8000.0f   /* 240 px / 30 mm */
#define HEX_PX_PER_M_Y     7100.0f   /* 284 px / 40 mm */
#define HEX_PHYS_DT        0.004f    /* 250 Hz kinematic substep */
#define HEX_MAX_SPEED      12000.0f
#define HEX_SHAKE_GAIN     1.0f
#define HEX_RADIUS         34.0f
#define HEX_LEFT           (WS_SAFE + 4)
#define HEX_RIGHT          (WS_SCREEN_W - WS_SAFE - 4)
#define HEX_TOP            48
#define HEX_BOTTOM         216
#define HEX_DRAG           0.16f     /* 1/s air/field drag */
#define HEX_TANGENT_DAMP   0.985f    /* per contact, no static capture */
#define HEX_SPIN_DAMP      0.995f    /* per rendered frame */

static int s_hex_g_level;
static float s_hex_x;
static float s_hex_y;
static float s_hex_vx;
static float s_hex_vy;
static float s_hex_angle;
static float s_hex_spin;
static float s_hex_gx;
static float s_hex_gy;
static bool s_hex_filter_ready;
static int64_t s_hex_last;
static float s_hex_vertex[6][2];

static float hex_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void hex_set_gravity(int level)
{
    if (level < 1) level = 1;
    if (level > HEX_G_MAX) level = HEX_G_MAX;
    if (level == s_hex_g_level) return;
    s_hex_g_level = level;
    watch_config_set_int(HEX_LEVEL_SAVE, s_hex_g_level);
}

static float hex_gravity_scale(void)
{
    return (float)s_hex_g_level / (float)HEX_G_MAX;
}

static void hex_place(void)
{
    s_hex_x = 120.0f;
    s_hex_y = 100.0f;
    s_hex_vx = 0.0f;
    s_hex_vy = 0.0f;
    s_hex_angle = 0.0f;
    s_hex_spin = 0.0f;
    s_hex_gx = 0.0f;
    s_hex_gy = 0.0f;
    s_hex_filter_ready = false;
    s_hex_last = 0;
}

static void hex_load(void)
{
    s_hex_g_level = watch_config_get_int(HEX_LEVEL_SAVE, HEX_G_MAX);
    if (s_hex_g_level < 1) s_hex_g_level = 1;
    if (s_hex_g_level > HEX_G_MAX) s_hex_g_level = HEX_G_MAX;
    hex_place();
}

static bool hex_read_motion(float *gx, float *gy, float *lax, float *lay)
{
    const ws_imu_t *imu = watch_sensors_imu();
    if (!imu->ok) {
        *gx = 0.0f;
        *gy = 1.0f;          /* sensor offline: fall down in screen space */
        *lax = 0.0f;
        *lay = 0.0f;
        return false;
    }
    /* Same mounted-axis remap used previously: sensor X is screen-down
     * and sensor Y is screen-right, with the vertical sign inverted. */
    float gx_raw = -imu->ay;
    float gy_raw = -imu->ax;
    if (!s_hex_filter_ready) {
        s_hex_gx = gx_raw;
        s_hex_gy = gy_raw;
        s_hex_filter_ready = true;
    } else {
        s_hex_gx = s_hex_gx * 0.82f + gx_raw * 0.18f;
        s_hex_gy = s_hex_gy * 0.82f + gy_raw * 0.18f;
    }

    /* High-pass acceleration is shake, not gravity. */
    *lax = gx_raw - s_hex_gx;
    *lay = gy_raw - s_hex_gy;

    float mag = sqrtf(s_hex_gx * s_hex_gx + s_hex_gy * s_hex_gy);
    float ogx = s_hex_gx;
    float ogy = s_hex_gy;
    if (mag > 1.0f) {
        ogx /= mag;
        ogy /= mag;
    }
    if (mag < 0.04f) {
        ogx = 0.0f;
        ogy = 0.0f;
    }
    *gx = ogx;
    *gy = ogy;
    return true;
}

static void hex_update_rotation(float dt)
{
    const ws_imu_t *imu = watch_sensors_imu();
    if (imu->ok) {
        float target = hex_clampf(-imu->gz, -360.0f, 360.0f);
        if (fabsf(target) < 2.0f) target = 0.0f;
        s_hex_spin = s_hex_spin * 0.92f + target * 0.08f;
    }
    s_hex_spin *= HEX_SPIN_DAMP;
    s_hex_angle += s_hex_spin * dt;
    if (s_hex_angle > 360.0f) s_hex_angle -= 360.0f;
    if (s_hex_angle < 0.0f) s_hex_angle += 360.0f;
}

static void hex_project(void)
{
    const float d2r = 0.017453292f;
    for (int i = 0; i < 6; i++) {
        float a = (s_hex_angle + i * 60.0f) * d2r;
        s_hex_vertex[i][0] = s_hex_x + cosf(a) * HEX_RADIUS;
        s_hex_vertex[i][1] = s_hex_y + sinf(a) * HEX_RADIUS;
    }
}

static void hex_step(float dt, float gx, float gy, float lax, float lay)
{
    float gscale = hex_gravity_scale();
    s_hex_vx += (gx * HEX_GRAVITY_MPS2 * gscale + lax * HEX_SHAKE_GAIN) *
                HEX_PX_PER_M_X * dt;
    s_hex_vy += (gy * HEX_GRAVITY_MPS2 * gscale + lay * HEX_SHAKE_GAIN) *
                HEX_PX_PER_M_Y * dt;

    float drag = 1.0f - HEX_DRAG * dt;
    s_hex_vx *= drag;
    s_hex_vy *= drag;
    float speed = sqrtf(s_hex_vx * s_hex_vx + s_hex_vy * s_hex_vy);
    if (speed > HEX_MAX_SPEED) {
        s_hex_vx = s_hex_vx / speed * HEX_MAX_SPEED;
        s_hex_vy = s_hex_vy / speed * HEX_MAX_SPEED;
    }

    s_hex_x += s_hex_vx * dt;
    s_hex_y += s_hex_vy * dt;
    hex_project();

    float minx = s_hex_vertex[0][0], maxx = s_hex_vertex[0][0];
    float miny = s_hex_vertex[0][1], maxy = s_hex_vertex[0][1];
    for (int i = 1; i < 6; i++) {
        if (s_hex_vertex[i][0] < minx) minx = s_hex_vertex[i][0];
        if (s_hex_vertex[i][0] > maxx) maxx = s_hex_vertex[i][0];
        if (s_hex_vertex[i][1] < miny) miny = s_hex_vertex[i][1];
        if (s_hex_vertex[i][1] > maxy) maxy = s_hex_vertex[i][1];
    }

    /* A small normal deadzone stops gravity-sized impacts from buzzing against
     * the field. Tangential velocity is only lightly damped, so the hexagon
     * slides freely and is never held in place by static friction. */
    if (minx < HEX_LEFT) {
        float impact = -s_hex_vx;
        s_hex_x += HEX_LEFT - minx;
        if (s_hex_vx < 0.0f) {
            s_hex_vx = impact > 280.0f ? impact * HEX_RESTITUTION : 0.0f;
        }
        s_hex_vy *= HEX_TANGENT_DAMP;
    }
    if (maxx > HEX_RIGHT) {
        float impact = s_hex_vx;
        s_hex_x -= maxx - HEX_RIGHT;
        if (s_hex_vx > 0.0f) {
            s_hex_vx = impact > 280.0f ? -impact * HEX_RESTITUTION : 0.0f;
        }
        s_hex_vy *= HEX_TANGENT_DAMP;
    }
    if (miny < HEX_TOP) {
        float impact = -s_hex_vy;
        s_hex_y += HEX_TOP - miny;
        if (s_hex_vy < 0.0f) {
            s_hex_vy = impact > 280.0f ? impact * HEX_RESTITUTION : 0.0f;
        }
        s_hex_vx *= HEX_TANGENT_DAMP;
    }
    if (maxy > HEX_BOTTOM) {
        float impact = s_hex_vy;
        s_hex_y -= maxy - HEX_BOTTOM;
        if (s_hex_vy > 0.0f) {
            s_hex_vy = impact > 280.0f ? -impact * HEX_RESTITUTION : 0.0f;
        }
        s_hex_vx *= HEX_TANGENT_DAMP;
    }
}

static void hex_tick(void)
{
    int64_t now = esp_timer_get_time();
    float gx = 0.0f, gy = 1.0f;
    float lax = 0.0f, lay = 0.0f;
    hex_read_motion(&gx, &gy, &lax, &lay);

    if (!s_hex_last) {
        s_hex_last = now;
        return;
    }
    float dt = (float)(now - s_hex_last) / 1000000.0f;
    s_hex_last = now;
    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;

    hex_update_rotation(dt);
    while (dt > 0.0001f) {
        float step = dt > HEX_PHYS_DT ? HEX_PHYS_DT : dt;
        hex_step(step, gx, gy, lax, lay);
        dt -= step;
    }
}


#define FLASK_WAVE_N 49
#define FLASK_WAVE_SPAN 110.0f
#define FLASK_INNER_L 24.0f
#define FLASK_INNER_R 216.0f
#define FLASK_INNER_TOP 62.0f
#define FLASK_INNER_BOTTOM 214.0f
#define FLASK_CX 120.0f
#define FLASK_CY 138.0f
#define FLASK_BOX_LEFT 18
#define FLASK_BOX_RIGHT 222
#define FLASK_BOX_TOP 56
#define FLASK_BOX_BOTTOM 220
#define FLASK_SUBSTEP 0.005f
#define FLASK_WAKE_G 0.04f
#define FLASK_SPRING_K 90.0f
#define FLASK_SPRING_COUPLE 130.0f
#define FLASK_SPRING_DAMP 1.8f
#define FLASK_SHAKE_DRIVE 8000.0f

/* One-dimensional shallow-water surface.  h[] is the vertical offset from the
 * resting level; gravity drives a target slope and the high-pass acceleration
 * injects the energy that makes the surface slosh when the watch is shaken. */
static float s_flask_h[FLASK_WAVE_N];
static float s_flask_v[FLASK_WAVE_N];
static float s_flask_gx;
static float s_flask_gy;
static float s_flask_shake;
static bool s_flask_filter_ready;
static int64_t s_flask_last;

static float flask_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float flask_wave_at(float u)
{
    float idx = (u + FLASK_WAVE_SPAN) / (2.0f * FLASK_WAVE_SPAN) *
                (FLASK_WAVE_N - 1);
    if (idx <= 0.0f) return s_flask_h[0];
    if (idx >= (float)(FLASK_WAVE_N - 1)) return s_flask_h[FLASK_WAVE_N - 1];
    int i = (int)idx;
    float f = idx - (float)i;
    return s_flask_h[i] * (1.0f - f) + s_flask_h[i + 1] * f;
}

static void flask_reset(void)
{
    memset(s_flask_h, 0, sizeof(s_flask_h));
    memset(s_flask_v, 0, sizeof(s_flask_v));
    s_flask_gx = 0.0f;
    s_flask_gy = 1.0f;
    s_flask_shake = 0.0f;
    s_flask_filter_ready = false;
    s_flask_last = 0;
}

static bool flask_read_motion(float *shake)
{
    const ws_imu_t *imu = watch_sensors_imu();
    if (!imu->ok) {
        s_flask_gx = 0.0f;
        s_flask_gy = 1.0f;
        *shake = 0.0f;
        return false;
    }
    /* Same mounted-axis remap as the hex: sensor X is screen-down and sensor
     * Y is screen-right, with the vertical accelerometer sign inverted. */
    float gx_raw = -imu->ay;
    float gy_raw = -imu->ax;
    if (!s_flask_filter_ready) {
        s_flask_gx = gx_raw;
        s_flask_gy = gy_raw;
        s_flask_filter_ready = true;
    } else {
        s_flask_gx = s_flask_gx * 0.55f + gx_raw * 0.45f;
        s_flask_gy = s_flask_gy * 0.55f + gy_raw * 0.45f;
    }
    float lax = gx_raw - s_flask_gx;
    float lay = gy_raw - s_flask_gy;
    float gmag = sqrtf(s_flask_gx * s_flask_gx + s_flask_gy * s_flask_gy);
    if (gmag > 0.10f) {
        /* Normal acceleration relative to the free surface: horizontal shake
         * when the watch is upright, vertical shake when it is face-up. */
        *shake = (lax * s_flask_gx + lay * s_flask_gy) / gmag;
    } else {
        *shake = lax;
    }
    return true;
}

static void flask_tick(void)
{
    int64_t now = esp_timer_get_time();
    float shake = 0.0f;
    if (!flask_read_motion(&shake)) shake = 0.0f;

    if (!s_flask_last) {
        s_flask_last = now;
        return;
    }
    float dt = (float)(now - s_flask_last) / 1000000.0f;
    s_flask_last = now;
    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;

    s_flask_shake = s_flask_shake * 0.45f + shake * 0.55f;

    while (dt > 0.0001f) {
        float step = dt > FLASK_SUBSTEP ? FLASK_SUBSTEP : dt;
        for (int i = 0; i < FLASK_WAVE_N; i++) {
            float u = ((float)i - (FLASK_WAVE_N - 1) * 0.5f) /
                      ((FLASK_WAVE_N - 1) * 0.5f) * FLASK_WAVE_SPAN;
            float left = i > 0 ? s_flask_h[i - 1] : s_flask_h[i];
            float right = i < FLASK_WAVE_N - 1 ? s_flask_h[i + 1] : s_flask_h[i];
            float lap = left + right - 2.0f * s_flask_h[i];
            float drive = s_flask_shake * FLASK_SHAKE_DRIVE * (u / FLASK_WAVE_SPAN);
            float accel = -FLASK_SPRING_K * s_flask_h[i] +
                          FLASK_SPRING_COUPLE * lap -
                          FLASK_SPRING_DAMP * s_flask_v[i] + drive;
            s_flask_v[i] = flask_clampf(s_flask_v[i] + accel * step,
                                        -1200.0f, 1200.0f);
            s_flask_h[i] = flask_clampf(s_flask_h[i] + s_flask_v[i] * step,
                                        -60.0f, 60.0f);
        }
        dt -= step;
    }
}

static void hex_button(int x, const char *label)
{
    watch_gfx_fill_rect(x, 246, 54, 26, 0x0100);
    watch_gfx_rect(x, 246, 54, 26, WS_COLOR_DIM);
    watch_gfx_text_center(x + 27, 251, label, WS_COLOR_GREEN, 2);
}

static bool hex_hit_minus(int x, int y)
{
    return x >= WS_SAFE && x < WS_SAFE + 54 && y >= 246 && y < 272;
}

static bool hex_hit_plus(int x, int y)
{
    int left = WS_SCREEN_W - WS_SAFE - 54;
    return x >= left && x < left + 54 && y >= 246 && y < 272;
}

static void hex_draw(void)
{
    const ws_imu_t *imu = watch_sensors_imu();
    bool imu_ok = imu->ok;
    float gmag = sqrtf(s_hex_gx * s_hex_gx + s_hex_gy * s_hex_gy);

    watch_gfx_text(WS_SAFE, 26, "HEX", WS_COLOR_CYAN, 1);
    watch_gfx_text(WS_SAFE + 30, 26, imu_ok ? "IMU" : "NO IMU",
                   imu_ok ? WS_COLOR_GREEN : WS_COLOR_RED, 1);
    watch_gfx_printf(WS_SCREEN_W - WS_SAFE - 58, 26, WS_COLOR_GREEN, 1,
                     "G %.1fx", hex_gravity_scale());
    if (!imu_ok) {
        watch_gfx_text_center(120, 36, "NO IMU / KINEMATIC", WS_COLOR_RED, 1);
    } else if (gmag < 0.04f) {
        watch_gfx_text_center(120, 36, "FLAT / TILT WATCH", WS_COLOR_AMBER, 1);
    } else {
        watch_gfx_text_center(120, 36, "2D HEX / 6 EDGES", WS_COLOR_DIM, 1);
    }

    watch_gfx_rect(HEX_LEFT - 2, HEX_TOP - 2,
                   HEX_RIGHT - HEX_LEFT + 4, HEX_BOTTOM - HEX_TOP + 4,
                   WS_COLOR_DIM);
    for (int x = HEX_LEFT; x <= HEX_RIGHT; x += 16) {
        watch_gfx_vline(x, HEX_TOP - 1, 2, 0x0120);
        watch_gfx_vline(x, HEX_BOTTOM, 2, 0x0120);
    }

    hex_project();
    for (int i = 0; i < 6; i++) {
        int a = i;
        int b = (i + 1) % 6;
        watch_gfx_line((int)s_hex_vertex[a][0], (int)s_hex_vertex[a][1],
                       (int)s_hex_vertex[b][0], (int)s_hex_vertex[b][1],
                       i == 0 ? WS_COLOR_WHITE : 0x03E0);
    }
    for (int i = 0; i < 6; i++) {
        int px = (int)s_hex_vertex[i][0];
        int py = (int)s_hex_vertex[i][1];
        watch_gfx_fill_rect(px - 1, py - 1, 2, 2, WS_COLOR_WHITE);
    }
    watch_gfx_line((int)s_hex_x, (int)s_hex_y,
                   (int)s_hex_vertex[0][0], (int)s_hex_vertex[0][1],
                   0x01A0);
    watch_gfx_fill_rect((int)s_hex_x - 1, (int)s_hex_y - 1, 2, 2, 0x02A0);

    watch_gfx_text(WS_SAFE, 230, "GRAVITY", WS_COLOR_GRAY, 1);
    watch_gfx_printf(WS_SAFE + 48, 230, WS_COLOR_GREEN, 1, "%.1fx",
                     hex_gravity_scale());
    const int bar_x = WS_SAFE + 76;
    for (int i = 0; i < HEX_G_MAX; i++) {
        uint16_t color = i < s_hex_g_level ? WS_COLOR_GREEN : 0x0120;
        watch_gfx_fill_rect(bar_x + i * 12, 232, 10, 5, color);
    }

    hex_button(WS_SAFE, "-");
    hex_button(WS_SCREEN_W - WS_SAFE - 54, "+");
    watch_gfx_text_center(120, 254, "SHAKE / TILT", WS_COLOR_DIM, 1);
}


static void flask_draw_frame(void)
{
    const uint16_t glass = 0x02A0;
    const uint16_t inner = 0x01A0;

    watch_gfx_rect(FLASK_BOX_LEFT, FLASK_BOX_TOP,
                   FLASK_BOX_RIGHT - FLASK_BOX_LEFT + 1,
                   FLASK_BOX_BOTTOM - FLASK_BOX_TOP + 1, glass);
    watch_gfx_rect(FLASK_BOX_LEFT + 4, FLASK_BOX_TOP + 4,
                   FLASK_BOX_RIGHT - FLASK_BOX_LEFT - 7,
                   FLASK_BOX_BOTTOM - FLASK_BOX_TOP - 7, inner);

    /* Centre crosshair and graduation marks: a closed spirit-level cell. */
    watch_gfx_vline((int)FLASK_CX, (int)FLASK_INNER_TOP,
                    (int)(FLASK_INNER_BOTTOM - FLASK_INNER_TOP), 0x0100);
    watch_gfx_hline((int)FLASK_INNER_L, (int)FLASK_CY,
                    (int)(FLASK_INNER_R - FLASK_INNER_L), 0x0100);
    for (int y = 78; y <= 202; y += 18) {
        watch_gfx_hline(208, y, 5, 0x01A0);
    }
}

static bool flask_surface_point(float t, float depth, float gx, float gy,
                                int *out_x, int *out_y)
{
    float dx = -gy;
    float dy = gx;
    float n = flask_wave_at(t) + depth;
    float x = FLASK_CX + dx * t + gx * n;
    float y = FLASK_CY + dy * t + gy * n;
    if (x < FLASK_INNER_L || x > FLASK_INNER_R ||
        y < FLASK_INNER_TOP || y > FLASK_INNER_BOTTOM) {
        return false;
    }
    *out_x = (int)(x + 0.5f);
    *out_y = (int)(y + 0.5f);
    return true;
}

static void flask_draw_contour(float depth, uint16_t color,
                               float gx, float gy)
{
    int prev_x = 0, prev_y = 0;
    bool have_prev = false;
    for (int t = -130; t <= 130; t += 2) {
        int x, y;
        if (flask_surface_point((float)t, depth, gx, gy, &x, &y)) {
            if (have_prev) watch_gfx_line(prev_x, prev_y, x, y, color);
            prev_x = x;
            prev_y = y;
            have_prev = true;
        } else {
            have_prev = false;
        }
    }
}

static void flask_draw(void)
{
    const ws_imu_t *imu = watch_sensors_imu();
    bool imu_ok = imu->ok;
    float gx = s_flask_gx;
    float gy = s_flask_gy;
    float gmag = sqrtf(gx * gx + gy * gy);
    if (gmag < FLASK_WAKE_G) {
        gx = 0.0f;
        gy = 1.0f;
    } else {
        gx /= gmag;
        gy /= gmag;
    }

    watch_gfx_text(WS_SAFE, 26, "FLASK", WS_COLOR_CYAN, 1);
    watch_gfx_text_right(WS_SCREEN_W - WS_SAFE, 26,
                         imu_ok ? "IMU" : "NO IMU",
                         imu_ok ? WS_COLOR_GREEN : WS_COLOR_RED, 1);

    const uint16_t water = 0x01A8;
    for (int y = (int)FLASK_INNER_TOP; y <= (int)FLASK_INNER_BOTTOM; y++) {
        float s = gx * (FLASK_INNER_L - FLASK_CX) + gy * (y - FLASK_CY);
        float u = -gy * (FLASK_INNER_L - FLASK_CX) + gx * (y - FLASK_CY);
        for (int x = (int)FLASK_INNER_L; x <= (int)FLASK_INNER_R; x++) {
            if (s > flask_wave_at(u)) watch_gfx_pixel(x, y, water);
            s += gx;
            u -= gy;
        }
    }

    /* Ripple bands inside the water make the wave shape visible even when the
     * watch is still; they move with the same slosh field as the surface. */
    flask_draw_contour(8.0f, 0x02AA, gx, gy);
    flask_draw_contour(16.0f, 0x01C0, gx, gy);
    flask_draw_contour(24.0f, 0x0120, gx, gy);
    flask_draw_contour(1.0f, 0x02AA, gx, gy);
    flask_draw_contour(0.0f, WS_COLOR_CYAN, gx, gy);

    /* Foam ticks on the local crests, not bubbles in the middle. */
    for (int i = 2; i < FLASK_WAVE_N - 2; i++) {
        if (s_flask_h[i] > 4.0f &&
            s_flask_h[i] >= s_flask_h[i - 1] &&
            s_flask_h[i] >= s_flask_h[i + 1] &&
            s_flask_h[i] >= s_flask_h[i - 2] &&
            s_flask_h[i] >= s_flask_h[i + 2]) {
            float t = ((float)i - (FLASK_WAVE_N - 1) * 0.5f) /
                      ((FLASK_WAVE_N - 1) * 0.5f) * FLASK_WAVE_SPAN;
            int x0, y0, x1, y1;
            if (flask_surface_point(t, 0.0f, gx, gy, &x0, &y0) &&
                flask_surface_point(t, -3.0f, gx, gy, &x1, &y1)) {
                watch_gfx_line(x0, y0, x1, y1, WS_COLOR_WHITE);
            }
        }
    }

    flask_draw_frame();

    watch_gfx_text(WS_SAFE, 232, "WATER", WS_COLOR_GRAY, 1);
    watch_gfx_printf(WS_SAFE + 42, 232, WS_COLOR_GREEN, 1, "%d%%", 50);
    watch_gfx_text_center(120, 250, "SHAKE + TILT", WS_COLOR_DIM, 1);
}

static void menu_draw(void)
{
    static const char *const names[] = { "SNAKE", "2048", "MATRIX", "HEX", "FLASK" };
    static const char *const desc[] = {
        "swipe to steer", "swipe to merge", "long string rain",
        "2D IMU hexagon", "IMU water level",
    };
    watch_gfx_text_center(120, 26, "GAMES", WS_COLOR_CYAN, 3);
    watch_gfx_text_center(120, 62, "LINE ART ARCADE", WS_COLOR_DIM, 1);
    for (int i = 0; i < GAMES_MENU_COUNT; i++) {
        int y = GAMES_MENU_Y0 + i * GAMES_MENU_STEP;
        bool sel = i == s_menu_sel;
        watch_gfx_rect(28, y, 184, GAMES_MENU_H, sel ? WS_COLOR_GREEN : WS_COLOR_DIM);
        if (sel) watch_gfx_fill_rect(29, y + 1, 182, GAMES_MENU_H - 2, 0x0220);
        watch_gfx_text_center(120, y + 4, names[i],
                              sel ? WS_COLOR_WHITE : WS_COLOR_GREEN, 2);
        watch_gfx_text_center(120, y + 20, desc[i], WS_COLOR_GRAY, 1);
    }
    watch_gfx_text_center(120, 256, "LEFT/RIGHT or TAP", WS_COLOR_DIM, 1);
    watch_gfx_text_center(120, 268, "PWR: back to dial", WS_COLOR_DIM, 1);
}

static void games_enter(void)
{
    s_mode = GAME_MENU;
    s_menu_sel = 0;
    watch_ui_set_chrome_hidden(false);
    snake_reset();
    grid_reset();
    hex_load();
    flask_reset();
}

static void games_leave(void)
{
    watch_ui_set_chrome_hidden(false);
}

static void touch_swipe(const ws_event_t *ev)
{
    if (abs(ev->dx) < 12 && abs(ev->dy) < 12) return;
    if (s_mode == GAME_SNAKE) {
        if (abs(ev->dx) > abs(ev->dy)) {
            if (ev->dx > 0 && s_snake_dir != 3) s_snake_dir = 1;
            if (ev->dx < 0 && s_snake_dir != 1) s_snake_dir = 3;
        } else {
            if (ev->dy > 0 && s_snake_dir != 0) s_snake_dir = 2;
            if (ev->dy < 0 && s_snake_dir != 2) s_snake_dir = 0;
        }
    } else if (s_mode == GAME_2048) {
        if (abs(ev->dx) > abs(ev->dy)) grid_move(ev->dx > 0 ? 2 : 0);
        else grid_move(ev->dy > 0 ? 3 : 1);
    }
}

static void games_event(const ws_event_t *ev)
{
    if (s_mode == GAME_MATRIX) {
        if (ev->type == WS_EV_TOUCH_UP) {
            s_mode = GAME_MENU;
            watch_ui_set_chrome_hidden(false);
        }
        return;
    }
    if (s_mode == GAME_HEX) {
        if (ev->type == WS_EV_KEY_DOWN) {
            if (ev->key == WS_KEY_LEFT) {
                hex_set_gravity(s_hex_g_level - 1);
            }
            if (ev->key == WS_KEY_RIGHT) {
                hex_set_gravity(s_hex_g_level + 1);
            }
            return;
        }
        if (ev->type == WS_EV_TOUCH_UP) {
            if (hex_hit_minus(ev->x, ev->y)) {
                hex_set_gravity(s_hex_g_level - 1);
            } else if (hex_hit_plus(ev->x, ev->y)) {
                hex_set_gravity(s_hex_g_level + 1);
            }
            /* No tap-to-reset for the hexagon either. */
            return;
        }
        return;
    }
    if (s_mode == GAME_FLASK) {
        /* The flask has no touch reset; tilt and shake alone drive the water. */
        return;
    }
    if (ev->type == WS_EV_KEY_DOWN) {
        if (s_mode == GAME_MENU) {
            if (ev->key == WS_KEY_LEFT) {
                s_menu_sel = (s_menu_sel + GAMES_MENU_COUNT - 1) % GAMES_MENU_COUNT;
            }
            if (ev->key == WS_KEY_RIGHT) {
                s_menu_sel = (s_menu_sel + 1) % GAMES_MENU_COUNT;
            }
        } else if (s_mode == GAME_2048) {
            if (ev->key == WS_KEY_LEFT) grid_move(0);
            if (ev->key == WS_KEY_RIGHT) grid_move(2);
        }
        return;
    }
    if (ev->type == WS_EV_TOUCH_UP) {
        if (s_mode == GAME_MENU) {
            if (ev->y >= GAMES_MENU_Y0 &&
                ev->y < GAMES_MENU_Y0 + GAMES_MENU_COUNT * GAMES_MENU_STEP) {
                int item = (ev->y - GAMES_MENU_Y0) / GAMES_MENU_STEP;
                if (item > GAMES_MENU_COUNT - 1) item = GAMES_MENU_COUNT - 1;
                s_menu_sel = item;
                if (item == 0) {
                    snake_reset();
                    s_mode = GAME_SNAKE;
                } else if (item == 1) {
                    grid_reset();
                    s_mode = GAME_2048;
                } else if (item == 2) {
                    rain_reset();
                    s_mode = GAME_MATRIX;
                    watch_ui_set_chrome_hidden(true);
                } else if (item == 3) {
                    hex_place();
                    s_mode = GAME_HEX;
                } else {
                    flask_reset();
                    s_mode = GAME_FLASK;
                }
            }
            return;
        }
        if ((s_mode == GAME_SNAKE && s_snake_dead) || (s_mode == GAME_2048 && s_2048_dead)) {
            s_mode = GAME_MENU;
            return;
        }
        touch_swipe(ev);
    }
}

static void games_tick(void)
{
    if (s_mode == GAME_MATRIX) {
        rain_step();
        return;
    }
    if (s_mode == GAME_HEX) {
        hex_tick();
        return;
    }
    if (s_mode == GAME_FLASK) {
        flask_tick();
        return;
    }
    if (s_mode != GAME_SNAKE || s_snake_dead) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_move < 170000) return;
    s_last_move = now;
    snake_step();
}

static void games_render(void)
{
    if (s_mode == GAME_MENU) menu_draw();
    else if (s_mode == GAME_SNAKE) snake_draw();
    else if (s_mode == GAME_MATRIX) rain_draw();
    else if (s_mode == GAME_HEX) hex_draw();
    else if (s_mode == GAME_FLASK) flask_draw();
    else game2048_draw();
}

const ws_app_impl_t ws_app_games = {
    .id = "games",
    .name = "GAMES",
    .subtitle = "snake/2048/matrix/hex/flask",
    .enter = games_enter,
    .leave = games_leave,
    .render = games_render,
    .event = games_event,
    .tick = games_tick,
};
