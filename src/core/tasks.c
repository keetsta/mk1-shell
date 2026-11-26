#include "tasks.h"

#include <string.h>

#include "sha256.h"

/* The text of gate k lives in chapter k-1, and its answer unlocks chapter k. */
const task_def TASKS[] = {
    {"lamp", 0x51a7c0de, 10, TASK_BTN, TASK_LEGACY},
    {"level", 0x2c4e9b31, 10, TASK_LUX, TASK_LEGACY},
    {"field sync", 0x67777413, 12, 0, TASK_FIELD_SYNC},
    {"shadow modem", 0x49b1b273, 12, 0, TASK_SHADOW_MODEM},
    {"missing line", 0x74583acb, 12, 0, TASK_MISSING_LINE},
    {"noise floor", 0x49b1b273, 12, 0, TASK_NOISE_FLOOR},
};
const int N_TASKS = (int)(sizeof TASKS / sizeof TASKS[0]);

#define SHADOW_DROP 48
#define SHADOW_LONG 19
#define PACKET_BITS 3
#define DATA_ROWS (VM_ROWS - 1)

static uint32_t lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static uint32_t case_seed(const task_def *t, unsigned index)
{
    return t->seed ^ (0x9e3779b9u * (index + 1));
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

static uint16_t reverse_row(uint16_t bits)
{
    uint16_t out = 0;
    for (int col = 0; col < 16; col++, bits >>= 1)
        out = (uint16_t)((out << 1) | (bits & 1u));
    return out;
}

static uint16_t calibration_row(unsigned index, int row, int parity)
{
    switch (index) {
    case 0: return (uint16_t)(0x2000u | (parity ? 0x80u >> (row % DATA_ROWS) : 0));
    case 1: return (uint16_t)(0x4000u >> row);
    default: return (uint16_t)(0x2000u | (row < 2 ? 0x1800u : 0x0400u >> (row & 1)));
    }
}

static void store_row(uint8_t *out, uint16_t row)
{
    out[0] = (uint8_t)(row >> 8);
    out[1] = (uint8_t)row;
}

static int display_fixture(const task_def *t, unsigned index, int example, vm_t *vm, uint16_t correct[VM_ROWS])
{
    int parity = t->kind == TASK_MISSING_LINE;
    if (!parity && t->kind != TASK_FIELD_SYNC)
        return 0;

    uint32_t seed = case_seed(t, index);
    int erased = (int)((index + 2) % DATA_ROWS);
    uint16_t checksum = 0;
    uint8_t *data = vm->dev.ee + parity;
    vm->dev.ea = 0;
    memset(vm->dev.ee, 0, sizeof vm->dev.ee);
    if (parity)
        vm->dev.ee[0] = (uint8_t)erased;
    for (int row = 0; row < VM_ROWS - parity; row++) {
        uint16_t bits = example ? calibration_row(index, row, parity) : (uint16_t)lcg(&seed);
        correct[row] = bits;
        if (parity) {
            checksum ^= bits;
            if (row == erased)
                bits = 0;
        } else if (row & 1) {
            bits = reverse_row(bits);
        }
        store_row(data + 2 * row, bits);
    }
    if (parity) {
        correct[DATA_ROWS] = 0;
        store_row(data + 2 * DATA_ROWS, checksum);
    }
    return 1;
}

int task_prepare(const task_def *t, unsigned index, vm_t *vm)
{
    uint16_t correct[VM_ROWS];
    return display_fixture(t, index % TASK_EXAMPLES, 1, vm, correct);
}

int task_example(const task_def *t, unsigned index, vm_t *vm)
{
    if (!task_prepare(t, index, vm))
        return 0;
    const uint8_t *data = vm->dev.ee + (t->kind == TASK_MISSING_LINE);
    for (int row = 0; row < VM_ROWS; row++, data += 2)
        vm->dev.screen[row] = (uint16_t)((data[0] << 8) | data[1]);
    return 1;
}

static uint16_t append_bit(uint16_t tape, unsigned bit)
{
    return (uint16_t)((tape << 1) | bit);
}

void task_observe(task_observer *state, uint16_t lux, uint16_t rows[VM_ROWS])
{
    if (!state->ready) {
        state->ready = 1;
        state->tape = 1;
        state->threshold = lux > SHADOW_DROP ? lux - SHADOW_DROP : lux / 2;
    }
    uint8_t dark = lux < state->threshold;
    if (dark) {
        if (state->duration < UINT16_MAX)
            state->duration++;
    } else if (state->dark) {
        state->tape = append_bit(state->tape, state->duration > SHADOW_LONG);
        state->duration = 0;
    }
    state->dark = dark;
    memset(rows, 0, VM_ROWS * sizeof *rows);
    rows[0] = state->tape;
    rows[1] = dark ? 0x8000 : 0;
}

typedef struct {
    task_result *res;
    const uint8_t (*expect)[4];
    uint8_t (*out)[4];
    uint8_t hash[32];
} checkpoints;

static int sample(checkpoints *check, const vm_t *vm, const uint16_t correct[VM_ROWS])
{
    if (vm->state == VM_ERR) {
        memcpy(check->res->err, vm->err, sizeof check->res->err);
        return 0;
    }
    if (correct) {
        for (int row = 0; row < VM_ROWS; row++) {
            if (vm->dev.screen[row] != correct[row]) {
                check->res->wrong_row = row;
                return 0;
            }
        }
    }

    uint8_t data[2 * VM_ROWS + 2], chk[32];
    for (int row = 0; row < VM_ROWS; row++)
        store_row(data + 2 * row, vm->dev.screen[row]);
    data[2 * VM_ROWS] = data[2 * VM_ROWS + 1] = 0; /* where the buzzer was, see tasks.h */
    tagged("mk1/smp", check->hash, 32, data, sizeof data, check->hash);
    tagged("mk1/chk", check->hash, 32, NULL, 0, chk);
    int index = check->res->passed;
    if (check->out)
        memcpy(check->out[index], chk, 4);
    if (check->expect && memcmp(check->expect[index], chk, 4))
        return 0;
    check->res->passed++;
    return 1;
}

static void run_input(vm_t *vm, task_result *res, unsigned ticks, uint8_t btn, uint16_t lux)
{
    uint32_t before = vm->cycles;
    unsigned elapsed = 0;
    vm->dev.btn = btn;
    vm->dev.lux = lux;
    while (elapsed < ticks && vm->state != VM_ERR) {
        vm_tick(vm);
        elapsed++;
    }
    if (res) {
        res->ticks += elapsed;
        res->cycles += vm->cycles - before;
    }
}

static void run_legacy(const task_def *t, vm_t *vm, checkpoints *check)
{
    uint32_t s = t->seed;
    uint8_t btn = lcg(&s) & 1;
    vm_reset(vm, (uint16_t)(t->seed >> 16));
    memset(vm->dev.ee, 0, sizeof vm->dev.ee);
    for (int seg = 0; seg < t->segs; seg++) {
        uint32_t len = 30 + lcg(&s) % 50;
        uint16_t lux = (t->vary & TASK_LUX) ? (uint16_t)(lcg(&s) % 1024) : (uint16_t)(300 + lcg(&s) % 200);
        if (seg && (t->vary & TASK_BTN))
            btn ^= 1;
        run_input(vm, check->res, len, (t->vary & TASK_BTN) ? btn : 0, lux);
        if (!sample(check, vm, NULL))
            return;
    }
}

static void run_display(const task_def *t, vm_t *vm, checkpoints *check)
{
    for (unsigned index = 0; index < t->segs; index++) {
        uint16_t correct[VM_ROWS];
        vm_reset(vm, 0);
        display_fixture(t, index, 0, vm, correct);
        run_input(vm, check->res, 30, 0, 512);
        if (!sample(check, vm, correct))
            return;
    }
}

static int optical_packet(const task_def *t, unsigned index, vm_t *vm, checkpoints *check)
{
    int noisy = check == NULL;
    uint32_t seed = case_seed(t, index + (noisy ? 256u : 0u));
    uint16_t ambient = (uint16_t)(240 + lcg(&seed) % 540);
    uint16_t correct[VM_ROWS] = {1};
    task_result *res = check ? check->res : NULL;
    /* The random port is independent of the fixture generator. */
    vm_reset(vm, 0);
    memset(vm->dev.ee, 0, sizeof vm->dev.ee);
    run_input(vm, res, 8, 0, ambient);

    for (int k = 0; k < PACKET_BITS; k++) {
        unsigned bit = lcg(&seed) & 1u;
        unsigned width = bit ? 24 + lcg(&seed) % 11 : 8 + lcg(&seed) % 7;
        uint16_t lit = ambient;
        if (noisy)
            lit = (uint16_t)(ambient + (int)(lcg(&seed) % 45) - 22);
        uint16_t dark = (uint16_t)(lit - (68 + lcg(&seed) % 135));
        unsigned gap = 9 + lcg(&seed) % 12;
        unsigned interference = noisy ? lcg(&seed) % 4 : 0;
        run_input(vm, res, gap, 0, lit);

        if (interference & 1u) {
            unsigned dip = 1 + lcg(&seed) % 4;
            uint16_t dim = (uint16_t)(lit - (25 + lcg(&seed) % 103));
            run_input(vm, res, dip, 0, dim);
            run_input(vm, res, 6, 0, lit);
        }
        if (interference & 2u) {
            unsigned flash = 1 + lcg(&seed) % 4;
            uint16_t glare = (uint16_t)(lit - lcg(&seed) % 80);
            run_input(vm, res, width / 2, 0, dark);
            run_input(vm, res, flash, 0, glare);
            run_input(vm, res, width - width / 2 - flash, 0, dark);
        } else {
            run_input(vm, res, width, 0, dark);
        }
        run_input(vm, res, 8, 0, lit);

        correct[0] = append_bit(correct[0], bit);
        if (check) {
            if (!sample(check, vm, correct))
                return 0;
        } else if (vm->state == VM_ERR || memcmp(vm->dev.screen, correct, sizeof correct)) {
            return 0;
        }
    }
    return 1;
}

void task_run(const task_def *t, vm_t *vm, const uint8_t (*expect)[4], uint8_t (*chk_out)[4], task_result *res)
{
    memset(res, 0, sizeof *res);
    res->segs = t->segs;
    res->wrong_row = -1;
    checkpoints check = {res, expect, chk_out, {0}};
    switch (t->kind) {
    case TASK_LEGACY: run_legacy(t, vm, &check); break;
    case TASK_FIELD_SYNC:
    case TASK_MISSING_LINE:
        run_display(t, vm, &check);
        break;
    case TASK_SHADOW_MODEM:
    case TASK_NOISE_FLOOR:
        for (unsigned index = 0; index < t->segs / PACKET_BITS; index++) {
            if (!optical_packet(t, index, vm, &check))
                return;
        }
        if (t->kind == TASK_NOISE_FLOOR) {
            for (unsigned index = 0; index < TASK_NOISE_PACKETS; index++)
                res->quality += (uint16_t)optical_packet(t, index, vm, NULL);
        }
        break;
    }
    if (res->passed == res->segs)
        memcpy(res->hash, check.hash, 32);
}

void task_key(const uint8_t answer[32], uint8_t key[32]) { tagged("mk1/key", answer, 32, NULL, 0, key); }

void chapter_crypt(const uint8_t key[32], const uint8_t *in, uint8_t *out, uint32_t len)
{
    uint8_t ks[32];
    for (uint32_t i = 0; i < len; i++) {
        if (i % 32 == 0) {
            uint8_t ctr[4] = {(uint8_t)(i >> 29), (uint8_t)(i >> 21), (uint8_t)(i >> 13), (uint8_t)(i >> 5)};
            tagged("mk1/txt", key, 32, ctr, 4, ks);
        }
        out[i] = in[i] ^ ks[i % 32];
    }
}
