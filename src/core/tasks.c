#include "tasks.h"

#include <string.h>

#include "sha256.h"

/* the text of task k lives in chapter k-1 of the manual; its answer unlocks chapter k */
const task_def TASKS[] = {
    {"lamp", 0x51a7c0de, 10, TASK_BTN},
    {"level", 0x2c4e9b31, 10, TASK_LUX},
};
const int N_TASKS = (int)(sizeof TASKS / sizeof TASKS[0]);

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void tagged(const char *tag, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, tag, strlen(tag));
    sha256_update(&c, a, alen);
    if (b)
        sha256_update(&c, b, blen);
    sha256_final(&c, out);
}

void task_run(const task_def *t, vm_t *vm, const uint8_t (*expect)[4], uint8_t (*chk_out)[4], task_result *res)
{
    uint32_t s = t->seed;
    uint8_t h[32] = {0};
    uint8_t btn = lcg(&s) & 1;

    memset(res, 0, sizeof *res);
    res->segs = t->segs;
    vm_reset(vm, (uint16_t)(t->seed >> 16));
    memset(vm->dev.ee, 0, sizeof vm->dev.ee);
    for (int seg = 0; seg < t->segs; seg++) {
        uint32_t len = 30 + lcg(&s) % 50;
        uint16_t lux = (t->vary & TASK_LUX) ? (uint16_t)(lcg(&s) % 1024) : (uint16_t)(300 + lcg(&s) % 200);
        if (seg && (t->vary & TASK_BTN))
            btn ^= 1;
        for (uint32_t k = 0; k < len; k++) {
            vm->dev.btn = (t->vary & TASK_BTN) ? btn : 0;
            vm->dev.lux = lux;
            vm_tick(vm);
            res->ticks++;
            if (vm->state == VM_ERR) {
                memcpy(res->err, vm->err, sizeof res->err);
                res->cycles = vm->cycles;
                return;
            }
        }
        uint8_t sample[2 * VM_ROWS + 2], chk[32];
        for (int r = 0; r < VM_ROWS; r++) {
            sample[2 * r] = (uint8_t)(vm->dev.screen[r] >> 8);
            sample[2 * r + 1] = (uint8_t)vm->dev.screen[r];
        }
        sample[2 * VM_ROWS] = (uint8_t)(vm->dev.bz >> 8);
        sample[2 * VM_ROWS + 1] = (uint8_t)vm->dev.bz;
        tagged("mc1/smp", h, 32, sample, sizeof sample, h);
        tagged("mc1/chk", h, 32, NULL, 0, chk);
        if (chk_out)
            memcpy(chk_out[seg], chk, 4);
        if (expect && memcmp(expect[seg], chk, 4)) {
            res->cycles = vm->cycles;
            return;
        }
        res->passed = seg + 1;
    }
    res->cycles = vm->cycles;
    memcpy(res->hash, h, 32);
}

void task_key(const uint8_t answer[32], uint8_t key[32]) { tagged("mc1/key", answer, 32, NULL, 0, key); }

void chapter_crypt(const uint8_t key[32], const uint8_t *in, uint8_t *out, uint32_t len)
{
    uint8_t ks[32];
    for (uint32_t i = 0; i < len; i++) {
        if (i % 32 == 0) {
            uint8_t ctr[4] = {(uint8_t)(i >> 29), (uint8_t)(i >> 21), (uint8_t)(i >> 13), (uint8_t)(i >> 5)};
            tagged("mc1/txt", key, 32, ctr, 4, ks);
        }
        out[i] = in[i] ^ ks[i % 32];
    }
}
