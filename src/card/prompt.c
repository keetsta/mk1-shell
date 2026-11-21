#include "prompt.h"

#include <math.h>

#include "../core/font.h"

#define FIRST_MS 2500u
#define EVERY_MS 9000u
#define STEP 3          /* ticks per column, as the shell's marquee */
#define BLINK_MS 530u

static const char ADDRESS[] = "keetsta.me/mk1";
static const uint8_t CHEVRON[7][2] = {{1, 1}, {2, 2}, {3, 3}, {4, 4}, {3, 5}, {2, 6}, {1, 7}};

static int running, asked, step;
static uint32_t since, next;
static uint16_t rows[9];
static marquee mq;

static void lit(float img[9][16], int x, int y, float v) {
    if (x >= 0 && x < 16 && y >= 0 && y < 9 && v > img[y][x]) img[y][x] = v;
}

static void cursor(float img[9][16], float v) {
    for (int y = 1; y <= 7; y++)
        for (int x = 7; x <= 9; x++) lit(img, x, y, v);
}

void prompt_start(uint32_t ms) {
    since = ms;
    running = asked = 0;
    next = ms + FIRST_MS;
}

void prompt_address(void) { asked = 1; }

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
    for (int i = 0; i < 7; i++) lit(img, CHEVRON[i][0], CHEVRON[i][1], 1);
    if ((ms - since) % (2 * BLINK_MS) < BLINK_MS) cursor(img, 1);
    return 0;
}

void prompt_sleep(float t, float img[9][16]) { cursor(img, 0.08f + 0.3f * (0.5f - 0.5f * cosf(t * 6.2831853f / 6.0f))); }
