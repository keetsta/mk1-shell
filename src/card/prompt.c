#include "prompt.h"

#include <math.h>

#include "../core/font.h"

#define FIRST_MS 2500u
#define EVERY_MS 9000u
#define STEP 3          /* ticks per column, as the shell's marquee */
#define BLINK_MS 530u

static const char ADDRESS[] = "keetsta.me/mk1";
/* a small chevron, 3 wide and 5 tall, its tip at (2, 2) */
static const uint8_t CHEVRON[5][2] = {{0, 0}, {1, 1}, {2, 2}, {1, 3}, {0, 4}};

static int running, asked, step;
static uint32_t since, next;
static uint16_t rows[9];
static marquee mq;

static void lit(float img[9][16], int x, int y, float v) {
    if (x >= 0 && x < 16 && y >= 0 && y < 9 && v > img[y][x]) img[y][x] = v;
}

static float ease(float u) { return u <= 0 ? 0 : u >= 1 ? 1 : 0.5f - 0.5f * cosf(u * 3.14159265f); }

/* the caret, t ms into the prompt: on 450 ms, out over 150, off 310, in over 150; a terminal's period with
   the edges eased, which the face's afterglow can't do: it only softens going out */
static float soft(uint32_t t) {
    uint32_t p = t % (2 * BLINK_MS);
    if (p < 450) return 1;
    if (p < 600) return 1 - ease((float)(p - 450) / 150.0f);
    if (p < 910) return 0;
    return ease((float)(p - 910) / 150.0f);
}

static void caret(float img[9][16], float v) {
    for (int y = 2; y <= 6; y++) lit(img, 14, y, v);
}

/* three small chevrons, dim, a light running through them left to right in the first half of every 2.6 s */
static void chevrons(uint32_t t, float img[9][16]) {
    float p = (float)(t % 2600) / 1300.0f, at = -3 + 17 * ease(p);
    for (int j = 0; j < 3; j++) {
        int x0 = 1 + j * 4;
        float d = (float)(x0 + 1) - at, v = 0.22f + (p < 1 ? 0.78f * expf(-d * d / 4.5f) : 0);
        for (int i = 0; i < 5; i++) lit(img, x0 + CHEVRON[i][0], 2 + CHEVRON[i][1], v);
    }
}

void prompt_start(uint32_t ms) {
    since = ms;
    running = asked = 0;
    next = ms + FIRST_MS;
}

void prompt_address(void) { asked = 1; }

int prompt_running(void) { return running; }

void prompt_idle(uint32_t age, float img[9][16]) {
    chevrons(age, img);
    caret(img, soft(age));
}

int prompt_draw(uint32_t ms, int linked, float img[9][16]) {
    if (!running && (asked || (!linked && (int32_t)(ms - next) >= 0))) {
        marquee_start(&mq, ADDRESS);
        marquee_step(&mq, rows);
        running = 1;
        step = 0;
    }
    asked = 0;
    if (running && ++step >= STEP) {
        step = 0;
        if (!marquee_step(&mq, rows)) {
            running = 0;
            next = ms + EVERY_MS;
        }
    }
    if (running) {
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 16; x++)
                if (rows[y] >> (15 - x) & 1) img[y][x] = 1;
        return 1;
    }
    prompt_idle(ms - since, img);
    return 0;
}

void prompt_sleep(float t, float img[9][16]) { caret(img, 0.08f + 0.3f * (0.5f - 0.5f * cosf(t * 6.2831853f / 6.0f))); }
