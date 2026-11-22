#include "saver.h"

#include <math.h>
#include <string.h>

#include "cube.h"

#define TAU 6.2831853f

static void dot(float img[9][16], int x, int y, float v) {
    if (x >= 0 && x < 16 && y >= 0 && y < 9 && v > img[y][x]) img[y][x] = v;
}

/* a point between pixels, shared by the four around it in light rather than in perceived level, so it keeps
   its brightness as it moves (a square root for the panel's 2.2 gamma: close, and one instruction) */
static void splat(float img[9][16], float x, float y, float v) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    dot(img, x0, y0, v * sqrtf((1 - fx) * (1 - fy)));
    dot(img, x0 + 1, y0, v * sqrtf(fx * (1 - fy)));
    dot(img, x0, y0 + 1, v * sqrtf((1 - fx) * fy));
    dot(img, x0 + 1, y0 + 1, v * sqrtf(fx * fy));
}

static float smooth(float a, float b, float v) {
    float s = (v - a) / (b - a);
    s = s < 0 ? 0 : s > 1 ? 1 : s;
    return s * s * (3 - 2 * s);
}

static uint32_t next(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* murmur3's finaliser: a seed with a pattern in it (a counter, a multiple) would make the LCG's first numbers
   follow the pattern */
static uint32_t mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ h >> 16;
}

/* sixteen pendulums, column i swinging 24 + i times every 48 s: in step at the start, then waves, snakes and
   mirrors until they line up again */
static void pendulums(uint32_t ms, float img[9][16]) {
    static const float trail[4] = {1, 0.45f, 0.2f, 0.08f};
    float now = (float)(ms % 48000u) / 1000.0f;
    for (int i = 0; i < 16; i++)
        for (int k = 3; k >= 0; k--) {
            float y = 4.0f + 3.6f * cosf(TAU * (float)(24 + i) * (now - 0.05f * (float)k) / 48.0f);
            splat(img, (float)i, y, trail[k]);
        }
}

/* an oscilloscope in XY mode: one beam, y running 1 to 2 times as fast as x and drifting back over 90 s, so
   each figure slowly turns into the next; the trace fades behind the beam */
static void scope(uint32_t ms, float img[9][16]) {
    float now = (float)(ms % 180000u) / 1000.0f;
    for (int s = 0; s < 200; s++) {
        float t = now - 0.01f * (float)s;
        float px = TAU * 0.5f * t, py = TAU * 0.5f * (1.5f * t - 0.5f * 90.0f / TAU * cosf(TAU * t / 90.0f));
        splat(img, 7.5f + 7.0f * sinf(px + 0.7f), 4.0f + 3.6f * sinf(py), 0.9f * expf(-(float)s / 100.0f));
    }
}

/* three soft blobs, each on its own slow loop; when two come close they run into one */
static void lava(uint32_t ms, float img[9][16]) {
    static const float turns[3][2] = {{41, 29}, {53, 37}, {67, 47}}; /* per 600 s, so 10 min loop seamlessly */
    static const float phase[3][2] = {{0, 1.3f}, {2.1f, 0.4f}, {4.2f, 2.7f}}, size[3] = {2.0f, 1.7f, 2.3f};
    float t = (float)(ms % 600000u) / 600000.0f, bx[3], by[3];
    for (int i = 0; i < 3; i++) {
        bx[i] = 7.5f + 5.5f * sinf(TAU * turns[i][0] * t + phase[i][0]);
        by[i] = 4.0f + 2.6f * sinf(TAU * turns[i][1] * t + phase[i][1]);
    }
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++) {
            float field = 0;
            for (int i = 0; i < 3; i++) {
                float dx = (float)x - bx[i], dy = ((float)y - by[i]) * 1.3f;
                field += size[i] * size[i] / (dx * dx + dy * dy + 0.5f);
            }
            dot(img, x, y, 0.65f * smooth(0.8f, 2.2f, field));
        }
}

/* the game of life on a torus, a generation every 0.3 s, cells fading in and out rather than blinking; a
   board that dies or settles into a loop is sown again */
static uint8_t cells[9][16];
static float shine[9][16];
static uint32_t sower, next_gen, seen[12];
static int stale;

static void sow(void) {
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++) cells[y][x] = next(&sower) % 100 < 35;
    stale = 0;
}

static void life(uint32_t ms, float img[9][16]) {
    if (ms >= next_gen) {
        static uint8_t grown[9][16];
        uint32_t hash = 2166136261u;
        int alive = 0, loops = 0;
        next_gen = ms + 300;
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 16; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        if (dx || dy) n += cells[(y + dy + 9) % 9][(x + dx + 16) % 16];
                grown[y][x] = n == 3 || (n == 2 && cells[y][x]);
                alive += grown[y][x];
                hash = (hash ^ grown[y][x]) * 16777619u;
            }
        memcpy(cells, grown, sizeof cells);
        for (int i = 0; i < 12; i++) loops |= seen[i] == hash;
        memmove(seen + 1, seen, sizeof seen - sizeof seen[0]);
        seen[0] = hash;
        if (!alive || (loops && ++stale > 20)) sow();
    }
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++) {
            shine[y][x] += ((cells[y][x] ? 0.85f : 0) - shine[y][x]) * 0.18f;
            dot(img, x, y, shine[y][x]);
        }
}

void saver_start(int kind, uint32_t seed) {
    if (kind == SAVER_LIFE) {
        sower = seed | 1;
        next_gen = 0;
        memset(shine, 0, sizeof shine);
        memset(seen, 0, sizeof seen);
        sow();
    }
}

void saver_draw(int kind, uint32_t ms, float img[9][16]) {
    switch (kind) {
    case SAVER_CUBE: cube_draw(ms, img); break;
    case SAVER_PENDULUMS: pendulums(ms, img); break;
    case SAVER_SCOPE: scope(ms, img); break;
    case SAVER_LAVA: lava(ms, img); break;
    case SAVER_LIFE: life(ms, img); break;
    }
}

/* ---- one after another ------------------------------------------------------ */

static int kind = -1;
static uint32_t dice, since;

/* another saver than the one before, the dice deciding which */
static void turn(uint32_t ms) {
    int pick = (int)(next(&dice) % (kind < 0 ? SAVER_N : SAVER_N - 1));
    if (kind >= 0 && pick >= kind) pick++;
    kind = pick;
    since = ms;
    saver_start(kind, next(&dice));
}

void saver_begin(uint32_t ms, uint32_t seed) {
    dice = mix(seed);
    kind = -1;
    turn(ms);
}

int saver_kind(void) { return kind; }

void saver_frame(uint32_t ms, float img[9][16]) {
    if (ms - since >= SAVER_TURN_MS) turn(ms);
    uint32_t t = ms - since, left = SAVER_TURN_MS - t;
    float fade = (float)(t < left ? t : left) / (float)SAVER_FADE_MS;
    saver_draw(kind, t, img);
    if (fade < 1)
        for (int y = 0; y < 9; y++)
            for (int x = 0; x < 16; x++) img[y][x] *= fade;
}
