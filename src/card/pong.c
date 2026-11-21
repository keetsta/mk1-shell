#include "pong.h"

#include <math.h>
#include <string.h>

#define DT 0.02f
#define HALF 1.5f        /* paddles are 3 rows */
#define REACH 1.6f       /* ball centre to paddle centre that still counts as a hit */
#define UP 8.0f          /* your paddle, rows per second: held */
#define DOWN 6.5f        /* and let go */
#define CARD_SPEED 3.8f  /* slower than a steep ball: the card can be beaten */
#define CARD_WATCH 8.0f  /* the card looks at the ball only in its own half */
#define SERVE_VX 5.5f
#define VX_MAX 11.0f
#define VY_MAX 9.0f
#define SPEEDUP 1.08f
#define ANGLE 2.2f       /* hitting off-centre steepens the return */

static const char *const DIGIT[10] = {
    "###"
    "#.#"
    "#.#"
    "#.#"
    "###",
    ".#."
    "##."
    ".#."
    ".#."
    "###",
    "###"
    "..#"
    "###"
    "#.."
    "###",
    "###"
    "..#"
    ".##"
    "..#"
    "###",
    "#.#"
    "#.#"
    "###"
    "..#"
    "..#",
    "###"
    "#.."
    "###"
    "..#"
    "###",
    "###"
    "#.."
    "###"
    "#.#"
    "###",
    "###"
    "..#"
    ".#."
    ".#."
    ".#.",
    "###"
    "#.#"
    "###"
    "#.#"
    "###",
    "###"
    "#.#"
    "###"
    "..#"
    "###",
};

static float rnd(pong *p) {
    uint32_t x = p->rnd;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p->rnd = x;
    return (float)(x >> 8) / 16777216.0f;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* from the centre, towards whoever just lost the point */
static void serve(pong *p, int to_you) {
    p->bx = 7.5f;
    p->by = 3.0f + 2.0f * rnd(p);
    p->vx = to_you ? -SERVE_VX : SERVE_VX;
    p->vy = (1.0f + 1.5f * rnd(p)) * (rnd(p) < 0.5f ? -1.0f : 1.0f);
    p->aim = 0;
    p->phase = PONG_SERVE;
    p->t = 0;
}

void pong_start(pong *p, uint32_t seed) {
    memset(p, 0, sizeof *p);
    p->rnd = seed ? seed : 0x6b2f1d3u;
    p->you = p->card = 4.0f;
    serve(p, 1);
}

static void point(pong *p, int you_scored) {
    p->score[you_scored ? 0 : 1]++;
    p->phase = p->score[0] >= PONG_WIN || p->score[1] >= PONG_WIN ? PONG_OVER : PONG_POINT;
    p->t = 0;
}

void pong_tick(pong *p, int button) {
    /* the 2 s to leave count only between rallies: holding the paddle up for long is normal play, and
       counted in a rally it threw a fair player out about twice a minute (host simulation) */
    p->held = button && p->phase != PONG_PLAY ? (uint16_t)(p->held + 1) : 0;
    if (p->held >= PONG_QUIT_TICKS) p->quit = 1;
    /* the score waits while the button is down, so a hold that runs on from the rally gets its 2 s */
    if (!(button && p->phase == PONG_POINT)) p->t += DT;
    p->you = clampf(p->you + (button ? -UP : DOWN) * DT, 1.0f, 7.0f);

    switch (p->phase) {
    case PONG_SERVE:
        if (p->t >= 0.7f) {
            p->phase = PONG_PLAY;
            p->t = 0;
        }
        break;
    case PONG_POINT:
        if (p->t >= 1.3f) serve(p, p->vx < 0);
        break;
    case PONG_OVER:
        if (p->t >= 2.6f) {
            p->score[0] = p->score[1] = 0;
            serve(p, 1);
        }
        break;
    }

    /* the card heads for where the ball is, not where it will be, and stays put while the ball is away:
       a player reacting in ~200 ms takes about 3 points in 5 (tests/test_card.c prints the count) */
    float target = p->phase != PONG_PLAY ? 4.0f : p->vx > 0 && p->bx > CARD_WATCH ? p->by + p->aim : p->card;
    p->card = clampf(p->card + clampf(target - p->card, -CARD_SPEED * DT, CARD_SPEED * DT), 1.0f, 7.0f);
    if (p->phase != PONG_PLAY) return;

    float x0 = p->bx;
    p->bx += p->vx * DT;
    p->by += p->vy * DT;
    if (p->by < 0) {
        p->by = -p->by;
        p->vy = -p->vy;
    } else if (p->by > 8) {
        p->by = 16 - p->by;
        p->vy = -p->vy;
    }
    if (p->vx < 0 && x0 > 1 && p->bx <= 1 && fabsf(p->by - p->you) <= REACH) {
        p->bx = 2 - p->bx;
        p->vx = -p->vx * SPEEDUP;
        p->vy += (p->by - p->you) * ANGLE;
        p->aim = (rnd(p) * 2 - 1) * 1.2f;
    } else if (p->vx > 0 && x0 < 14 && p->bx >= 14 && fabsf(p->by - p->card) <= REACH) {
        p->bx = 28 - p->bx;
        p->vx = -p->vx * SPEEDUP;
        p->vy += (p->by - p->card) * ANGLE;
    }
    p->vx = clampf(p->vx, -VX_MAX, VX_MAX);
    p->vy = clampf(p->vy, -VY_MAX, VY_MAX);
    if (p->bx < -0.5f)
        point(p, 0);
    else if (p->bx > 15.5f)
        point(p, 1);
}

static void lit(float img[9][16], int x, int y, float v) {
    if (x >= 0 && x < 16 && y >= 0 && y < 9 && v > img[y][x]) img[y][x] = v;
}

static void paddle(float img[9][16], int x, float c) {
    for (int y = 0; y < 9; y++) {
        float lo = fmaxf((float)y - 0.5f, c - HALF), hi = fminf((float)y + 0.5f, c + HALF);
        if (hi > lo) lit(img, x, y, hi - lo);
    }
}

/* spread over the four nearest LEDs so it glides, but scaled so its nearest LED is fully lit: split evenly,
   a plain spread leaves two quarter-lit LEDs that read as a dimmer ball */
static void ball(float img[9][16], float x, float y) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0, w[2][2], top = 0;
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            w[dy][dx] = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
            top = fmaxf(top, w[dy][dx]);
        }
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) lit(img, x0 + dx, y0 + dy, powf(w[dy][dx] / top, 0.7f));
}

static void digit(float img[9][16], int d, int x0, int y0) {
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 3; x++)
            if (DIGIT[d % 10][y * 3 + x] == '#') lit(img, x0 + x, y0 + y, 1);
}

static void score(const pong *p, float img[9][16]) {
    digit(img, p->score[0], 3, 2);
    lit(img, 7, 3, 1);
    lit(img, 7, 5, 1);
    digit(img, p->score[1], 9, 2);
}

void pong_draw(const pong *p, float img[9][16]) {
    memset(img, 0, sizeof(float) * 9 * 16);
    if (p->phase == PONG_POINT) {
        score(p, img);
    } else if (p->phase == PONG_OVER) {
        if (fmodf(p->t, 0.5f) < 0.3f) score(p, img);
    } else {
        paddle(img, 0, p->you);
        paddle(img, 15, p->card);
        if (p->phase == PONG_PLAY || fmodf(p->t, 0.25f) < 0.15f) ball(img, p->bx, p->by);
    }
    /* holding to leave: a bar fills the bottom row, so the gesture explains itself */
    if (p->held > 30) {
        float w = (float)(p->held - 30) / (PONG_QUIT_TICKS - 30) * 16.0f;
        for (int x = 0; x < 16 && (float)x < w; x++) lit(img, x, 8, 0.45f);
    }
}
