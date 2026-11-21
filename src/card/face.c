#include "face.h"

#include <math.h>
#include <string.h>

#include "own.h"

#define BOOT_MS 700u
#define LINK_MS 900u

static int mode, exit_req, btn, linked, running;
static uint32_t now, since, touched;
static uint16_t shell_rows[9];
static float img[9][16], glow[9][16];

static void enter(int m) {
    mode = m;
    since = now;
    if (m == FACE_OWN) own_enter(now);
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

static void lit(int x, int y, float v) {
    if (x >= 0 && x < 16 && y >= 0 && y < 9 && v > img[y][x]) img[y][x] = v;
}

static void bits(const uint16_t rows[9]) {
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++)
            if (rows[y] >> (15 - x) & 1) img[y][x] = 1;
}

/* a bright column sweeps across, its tail fading: every LED lights once at power-up */
static void draw_boot(float t) {
    float head = t / 0.55f * 20.0f - 2.0f;
    for (int x = 0; x < 16; x++) {
        float d = head - (float)x;
        if (d >= 0 && d < 4)
            for (int y = 0; y < 9; y++) lit(x, y, 1 - d * 0.25f);
    }
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
    case FACE_OWN:
        keep = own_draw(img);
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
    int was_linked = linked, was_running = running;
    linked = in->linked;
    running = in->running;
    if (pressed || in->typed || linked != was_linked) touched = now;

    if (mode == FACE_BOOT) {
        if (now - since >= BOOT_MS) enter(linked ? FACE_LINK : FACE_OWN);
    } else {
        if (linked && !was_linked)
            enter(FACE_LINK);
        else if (!linked && was_linked && !running)
            enter(FACE_OWN);
        else if (in->typed && linked && mode != FACE_SHELL && mode != FACE_LINK)
            enter(FACE_SHELL);
        if (exit_req && (mode == FACE_SHELL || mode == FACE_LINK)) enter(FACE_OWN);
        /* a program left running with no terminal keeps the screen until it stops */
        if (mode == FACE_SHELL && !linked && was_running && !running) enter(FACE_OWN);

        if (mode == FACE_LINK && now - since >= LINK_MS) {
            enter(FACE_SHELL);
        } else if (mode == FACE_OWN) {
            /* a press already down, or one coming as the card gets the screen back, belongs to the screen
               before */
            own_in o = {now, now - touched, (uint8_t)btn, (uint8_t)(pressed && since != now), (uint8_t)released,
                        (uint8_t)linked};
            own_tick(&o);
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
