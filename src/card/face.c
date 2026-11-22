#include "face.h"

#include <math.h>
#include <string.h>

#include "own.h"
#include "prompt.h"

#define BOOT_MS 620u
#define LINK_MS 300u
#define RETURN_MS 260u
#define PI 3.14159265f

static int mode, exit_req, btn, linked, running;
static uint32_t now, since, touched;
static uint16_t shell_rows[9];
static float img[9][16], glow[9][16], idle_frame[9][16], transition_frame[9][16];

static void enter(int m) {
    mode = m;
    since = now;
    if (m == FACE_OWN) own_enter(now);
    if (m == FACE_LINK || m == FACE_RETURN)
        memcpy(transition_frame, img, sizeof transition_frame);
}

void face_init(uint32_t ms) {
    now = touched = ms;
    btn = linked = running = exit_req = 0;
    memset(img, 0, sizeof img);
    memset(glow, 0, sizeof glow);
    memset(shell_rows, 0, sizeof shell_rows);
    memset(idle_frame, 0, sizeof idle_frame);
    prompt_idle(0, idle_frame);
    enter(FACE_BOOT);
}

void face_shell(const uint16_t rows[9]) { memcpy(shell_rows, rows, sizeof shell_rows); }
void face_exit(void) { exit_req = 1; }
int face_mode(void) { return mode; }

static void lit(int x, int y, float v) {
    if (x >= 0 && x < 16 && y >= 0 && y < 9 && v > img[y][x]) img[y][x] = v;
}

static void plot(float x, float y, float v) {
    if (v <= 0 || x <= -1 || x >= 16 || y <= -1 || y >= 9) return;
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    lit(ix, iy, v * (1 - fx) * (1 - fy));
    lit(ix + 1, iy, v * fx * (1 - fy));
    lit(ix, iy + 1, v * (1 - fx) * fy);
    lit(ix + 1, iy + 1, v * fx * fy);
}

static float ramp(float t, float start, float end) {
    float u = (t - start) / (end - start);
    return u <= 0 ? 0 : u >= 1 ? 1 : 0.5f - 0.5f * cosf(PI * u);
}

static float mix(float a, float b, float u) {
    return a + (b - a) * u;
}

static void bits(const uint16_t rows[9]) {
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++)
            if (rows[y] >> (15 - x) & 1) img[y][x] = 1;
}

static void draw_boot(float t) {
    float head = mix(-2.4f, 15.1f, ramp(t, 0, 316));
    float pulse = ramp(t, 0, 64) * (1 - ramp(t, 254, 316));
    plot(head, 4, 0.95f * pulse);
    plot(head - 1, 4, 0.49f * pulse);
    plot(head - 2, 4, 0.19f * pulse);

    static const float latch[3] = {65, 120, 177};
    for (int mark = 0; mark < 4; mark++) {
        float charge, spread, settle, initial;
        if (mark < 3) {
            float delay = latch[mark];
            charge = ramp(t, delay, delay + 35);
            spread = ramp(t, delay + 24, delay + 175);
            spread += 0.12f * sinf(PI * spread);
            settle = ramp(t, delay + 129, delay + 344);
            initial = 0.86f;
        } else {
            charge = ramp(t, 306, 371);
            spread = settle = ramp(t, 318, 465);
            initial = 0.70f;
        }
        for (int y = 0; y < 9; y++)
            for (int x = 4 * mark; x < 4 * (mark + 1); x++) {
                float target = idle_frame[y][x];
                if (target > 0)
                    plot((float)x, 4 + (float)(y - 4) * spread, mix(initial, target, settle) * charge);
            }
    }
}

static float pull_progress(float t) {
    return fminf(1, fmaxf(0, (t - 58) / 218));
}

static float pull_drift(float progress) {
    return 19 * powf(progress, 1.65f);
}

static void draw_pull(float t, int returning) {
    float preload = ramp(t, 0, 62) * (1 - ramp(t, 62, 155));
    float progress = pull_progress(t);
    float stretch = sinf(PI * progress);
    float spacing = 1 - 0.095f * preload;
    float offset = 7.5f * (1 - spacing) - 0.73f * preload + pull_drift(progress);
    float gain = 1 + 0.42f * stretch;
    if (returning) {
        float shift = offset - pull_drift(pull_progress(RETURN_MS));
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 16; x++)
                plot((float)x + shift, (float)y, transition_frame[y][x]);
    }
    float (*frame)[16] = returning ? idle_frame : transition_frame;
    for (int y = 0; y < 9; y++) {
        float dest_y = 4 + (float)(y - 4) * (1 - 0.49f * stretch);
        float tip = fmaxf(0, 2 - fabsf((float)(y - 4))) * 0.43f * stretch;
        for (int x = 0; x < 16; x++) {
            float dest_x = (float)x * spacing + offset + (x < 12 ? tip : 0);
            plot(dest_x, dest_y, fminf(1, frame[y][x] * gain));
        }
    }
}

static void draw(void) {
    float t = (float)(now - since), keep = 0;
    memset(img, 0, sizeof img);
    switch (mode) {
    case FACE_BOOT:
        draw_boot(t);
        break;
    case FACE_OWN:
        keep = own_draw(img);
        break;
    case FACE_LINK:
        draw_pull(t, 0);
        break;
    case FACE_RETURN:
        draw_pull(RETURN_MS - t, 1);
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
        else if (in->typed && linked && !exit_req && mode != FACE_SHELL && mode != FACE_LINK)
            enter(FACE_SHELL);
        /* a program left running with no terminal keeps the screen until it stops */
        if (mode == FACE_SHELL || mode == FACE_LINK) {
            if (exit_req || (!linked && !running && (was_linked || was_running)))
                enter(FACE_RETURN);
        } else if (mode == FACE_OWN && !linked && was_linked && !running) {
            enter(FACE_OWN);
        }

        if (mode == FACE_LINK && now - since >= LINK_MS) {
            enter(FACE_SHELL);
        } else if (mode == FACE_RETURN && now - since >= RETURN_MS) {
            enter(FACE_OWN);
        } else if (mode == FACE_OWN) {
            /* a press already down, or one coming as the card gets the screen back, belongs to the screen
               before */
            own_in o = {now, now - touched, (uint8_t)btn, (uint8_t)(pressed && since != now), (uint8_t)released,
                        (uint8_t)linked, in->rnd};
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
