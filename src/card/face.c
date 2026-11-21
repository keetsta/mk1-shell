#include "face.h"

#include <math.h>
#include <string.h>

#include "../core/font.h"
#include "cube.h"

#define BOOT_MS 700u
#define LINK_MS 900u
#define SAVER_MS (5u * 60u * 1000u)
#define SLEEP_MS (60u * 60u * 1000u)
#define GAME_LEFT_MS (45u * 1000u)  /* nobody pressed for this long: the game was abandoned */
#define HINT_FIRST_MS 2500u
#define HINT_EVERY_MS 9000u
#define HINT_STEP 3                 /* ticks per column, as the shell's marquee */
#define BLINK_MS 530u
#define HOLD_MS 2000u               /* held this long in idle, the button moves on to the next screen */
#define BAR_MS 600u                 /* and from here the bottom row shows it filling */

static const char HINT[] = "keetsta.me/mk1";
static const uint8_t CHEVRON[7][2] = {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {3, 5}, {2, 6}, {1, 7}};

static int mode, exit_req, btn, armed, linked, running, hint_on, hint_div;
static uint32_t now, since, touched, hint_at, down_at;
static uint16_t shell_rows[9], hint_rows[9];
static float img[9][16], glow[9][16];
static pong game;
static marquee hint;

static void enter(int m) {
    mode = m;
    since = now;
    hint_on = 0;
    hint_at = now + HINT_FIRST_MS;
    /* a press already down belongs to the screen it began on */
    armed = 0;
    if (m == FACE_GAME) pong_start(&game, now * 2654435761u + 1);
}

void face_init(uint32_t ms) {
    now = touched = ms;
    btn = linked = running = exit_req = 0;
    memset(glow, 0, sizeof glow);
    memset(shell_rows, 0, sizeof shell_rows);
    enter(FACE_BOOT);
}

void face_shell(const uint16_t rows[9]) { memcpy(shell_rows, rows, sizeof shell_rows); }
void face_exit(void) { exit_req = 1; }
int face_mode(void) { return mode; }
const pong *face_game(void) { return &game; }

static void lit(int x, int y, float v) {
    if (x >= 0 && x < 16 && y >= 0 && y < 9 && v > img[y][x]) img[y][x] = v;
}

static void bits(const uint16_t rows[9]) {
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++)
            if (rows[y] >> (15 - x) & 1) img[y][x] = 1;
}

static void cursor(float v) {
    for (int y = 1; y <= 7; y++)
        for (int x = 7; x <= 9; x++) lit(x, y, v);
}

/* a bright column sweeps across, its tail fading */
static void draw_boot(float t) {
    float head = t / 0.55f * 20.0f - 2.0f;
    for (int x = 0; x < 16; x++) {
        float d = head - (float)x;
        if (d >= 0 && d < 4)
            for (int y = 0; y < 9; y++) lit(x, y, 1 - d * 0.25f);
    }
}

/* a prompt and a blinking cursor; every few seconds the address runs by, unless a terminal is already there */
static void draw_idle(void) {
    if (!hint_on && !linked && (int32_t)(now - hint_at) >= 0) {
        marquee_start(&hint, HINT);
        marquee_step(&hint, hint_rows);
        hint_on = 1;
        hint_div = 0;
    }
    if (hint_on && ++hint_div >= HINT_STEP) {
        hint_div = 0;
        if (!marquee_step(&hint, hint_rows)) {
            hint_on = 0;
            hint_at = now + HINT_EVERY_MS;
        }
    }
    if (hint_on) {
        bits(hint_rows);
        return;
    }
    for (int i = 0; i < 7; i++) lit(CHEVRON[i][0], CHEVRON[i][1], 1);
    if ((now - since) % (2 * BLINK_MS) < BLINK_MS) cursor(1);
}

/* holding in idle: the bottom row fills, and when it is full the next screen comes */
static void draw_hold(void) {
    if (!armed || !btn || now - down_at < BAR_MS) return;
    float w = (float)(now - down_at - BAR_MS) / (float)(HOLD_MS - BAR_MS) * 16.0f;
    for (int x = 0; x < 16 && (float)x < w; x++) lit(x, 8, 0.45f);
}

/* someone opened the port: two dots meet in the middle and burst into a ring */
static void draw_link(float t) {
    if (t < 0.4f) {
        float a = t / 0.4f * 7.0f, f = a - floorf(a);
        int i = (int)a;
        lit(i, 4, 1 - f);
        lit(i + 1, 4, f);
        lit(15 - i, 4, 1 - f);
        lit(14 - i, 4, f);
        return;
    }
    float r = (t - 0.4f) / 0.5f * 10.0f;
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++) {
            float d = hypotf((float)x - 7.5f, (float)y - 4.0f);
            lit(x, y, 1 - fabsf(d - r) / 0.9f);
        }
}

static void draw(void) {
    float t = (float)(now - since) / 1000.0f, keep = 0;
    memset(img, 0, sizeof img);
    switch (mode) {
    case FACE_BOOT:
        draw_boot(t);
        break;
    case FACE_IDLE:
        /* the cursor blinks soft; the scrolling address gets none, any trail smears the letters */
        draw_idle();
        draw_hold();
        keep = hint_on ? 0 : 0.5f;
        break;
    case FACE_GAME:
        pong_draw(&game, img);
        keep = 0.6f;
        break;
    case FACE_SAVER: {
        uint8_t lum[9][16];
        cube_render((float)((now - since) % CUBE_PERIOD_MS) / 1000.0f, lum);
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 16; x++) img[y][x] = lum[y][x] / 255.0f;
        break;
    }
    case FACE_SLEEP:
        cursor(0.08f + 0.3f * (0.5f - 0.5f * cosf(t * 6.2831853f / 6.0f)));
        break;
    case FACE_LINK:
        draw_link(t);
        keep = 0.5f;
        break;
    case FACE_SHELL:
        /* exact: the player's own pixels, no afterglow */
        bits(shell_rows);
        break;
    }
    /* afterglow: what was lit fades over a few ticks instead of snapping off */
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++) {
            float g = glow[y][x] * keep;
            if (g > img[y][x]) img[y][x] = g;
            glow[y][x] = img[y][x];
        }
}

void face_tick(const face_in *in) {
    now = in->ms;
    int pressed = in->button && !btn, released = !in->button && btn;
    btn = in->button;
    if (pressed) {
        armed = 1;
        down_at = now;
    }
    int was_linked = linked, was_running = running;
    linked = in->linked;
    running = in->running;
    if (pressed || in->typed || linked != was_linked) touched = now;

    if (mode == FACE_BOOT) {
        if (now - since >= BOOT_MS) enter(linked ? FACE_LINK : FACE_IDLE);
    } else {
        if (linked && !was_linked)
            enter(FACE_LINK);
        else if (!linked && was_linked && !running)
            enter(FACE_IDLE);
        else if (in->typed && linked && mode != FACE_SHELL && mode != FACE_LINK)
            enter(FACE_SHELL);
        if (exit_req && (mode == FACE_SHELL || mode == FACE_LINK)) enter(FACE_IDLE);
        /* a program left running with no terminal keeps the screen until it stops */
        if (mode == FACE_SHELL && !linked && was_running && !running) enter(FACE_IDLE);

        switch (mode) {
        case FACE_LINK:
            if (now - since >= LINK_MS) enter(FACE_SHELL);
            break;
        case FACE_IDLE:
            /* a tap starts pong and a 2 s hold moves on to the cube, so the game waits for the button to
               come up */
            if (armed && btn && now - down_at >= HOLD_MS)
                enter(FACE_SAVER);
            else if (armed && released)
                enter(FACE_GAME);
            else if (now - touched >= SAVER_MS)
                enter(FACE_SAVER);
            break;
        case FACE_SAVER:
            if (pressed)
                enter(FACE_IDLE);
            else if (now - touched >= SLEEP_MS)
                enter(FACE_SLEEP);
            break;
        case FACE_SLEEP:
            if (pressed) enter(FACE_IDLE);
            break;
        case FACE_GAME:
            pong_tick(&game, btn);
            if (game.quit || now - touched >= GAME_LEFT_MS) enter(FACE_IDLE);
            break;
        }
    }
    exit_req = 0;
    draw();
}

void face_render(uint8_t lum[9][16]) {
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++) {
            float v = img[y][x];
            lum[y][x] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
        }
}
