#include "demo.h"

#include "../src/card/prompt.h"

#define SAVER_MS (5u * 60u * 1000u)
#define SLEEP_MS (60u * 60u * 1000u)
#define GALLERY_MS 15000u
#define GAME_LEFT_MS (45u * 1000u)  /* nobody pressed for this long: the game was abandoned */
#define HOLD_MS 2000u
#define BAR_MS 600u

static int mode, linked, btn, armed, saver, gallery;
static uint32_t now, since, down_at;
static pong game;

static void go(int m) {
    mode = m;
    since = now;
    /* a press already down belongs to the screen it began on */
    armed = 0;
    if (m == OWN_IDLE) prompt_start(now);
    if (m == DEMO_GAME) pong_start(&game, now * 2654435761u + 1);
}

void own_enter(uint32_t ms) {
    now = ms;
    go(OWN_IDLE);
}

int own_mode(void) { return mode; }
int demo_saver_kind(void) { return saver; }
const pong *demo_game(void) { return &game; }

static void show_saver(int kind, uint32_t seed) {
    saver = kind;
    saver_start(saver, seed);
    go(OWN_SAVER);
}

void own_tick(const own_in *in) {
    now = in->ms;
    linked = in->linked;
    btn = in->button;
    if (in->pressed) {
        armed = 1;
        down_at = now;
    }
    switch (mode) {
    case OWN_IDLE:
    case OWN_SAVER:
        if (btn) break;
        if (armed && in->released) {
            gallery = 0;
            if (now - down_at >= HOLD_MS)
                go(DEMO_GAME);
            else if (mode == OWN_IDLE)
                show_saver(SAVER_CUBE, in->rnd);
            else if (saver + 1 == SAVER_N)
                go(OWN_IDLE);
            else
                show_saver(saver + 1, in->rnd);
        } else if (in->still >= SLEEP_MS) {
            go(OWN_SLEEP);
        } else if (mode == OWN_IDLE && in->still >= SAVER_MS) {
            gallery = 1;
            show_saver(SAVER_CUBE, in->rnd);
        } else if (mode == OWN_SAVER && gallery && now - since >= GALLERY_MS) {
            show_saver((saver + 1) % SAVER_N, in->rnd);
        }
        break;
    case OWN_SLEEP:
        if (in->pressed) go(OWN_IDLE);
        break;
    case DEMO_GAME:
        pong_tick(&game, btn);
        if (game.quit || in->still >= GAME_LEFT_MS) go(OWN_IDLE);
        break;
    }
}

static void draw_hold(float img[9][16]) {
    if (!armed || !btn || now - down_at < BAR_MS) return;
    float w = (float)(now - down_at - BAR_MS) / (float)(HOLD_MS - BAR_MS) * 16.0f;
    for (int x = 0; x < 16 && (float)x < w; x++)
        if (img[8][x] < 0.45f) img[8][x] = 0.45f;
}

float own_draw(float img[9][16]) {
    switch (mode) {
    case OWN_IDLE:
        /* no afterglow: the caret fades itself, and any trail smears the address's letters */
        prompt_draw(now, linked, img);
        draw_hold(img);
        break;
    case DEMO_GAME:
        pong_draw(&game, img);
        return 0.6f;
    case OWN_SAVER:
        saver_draw(saver, now - since, img);
        draw_hold(img);
        break;
    case OWN_SLEEP:
        prompt_sleep((float)(now - since) / 1000.0f, img);
        break;
    }
    return 0;
}
