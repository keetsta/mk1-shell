#include "demo.h"

#include "../src/card/cube.h"
#include "../src/card/prompt.h"

#define SAVER_MS (5u * 60u * 1000u)
#define SLEEP_MS (60u * 60u * 1000u)
#define GAME_LEFT_MS (45u * 1000u)  /* nobody pressed for this long: the game was abandoned */
#define HOLD_MS 2000u               /* held this long on the prompt, the button moves on to the cube */
#define BAR_MS 600u                 /* and from here the bottom row shows it filling */

static int mode, linked, btn, armed;
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
const pong *demo_game(void) { return &game; }

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
        /* a tap starts pong and a 2 s hold moves on to the cube, so the game waits for the button to come up */
        if (armed && btn && now - down_at >= HOLD_MS)
            go(OWN_SAVER);
        else if (armed && in->released)
            go(DEMO_GAME);
        else if (in->still >= SAVER_MS)
            go(OWN_SAVER);
        break;
    case OWN_SAVER:
        if (in->pressed)
            go(OWN_IDLE);
        else if (in->still >= SLEEP_MS)
            go(OWN_SLEEP);
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

/* holding on the prompt: the bottom row fills, and when it is full the cube comes */
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
        cube_draw(now - since, img);
        break;
    case OWN_SLEEP:
        prompt_sleep((float)(now - since) / 1000.0f, img);
        break;
    }
    return 0;
}
