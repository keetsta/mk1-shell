#include <stddef.h>
#include <stdint.h>

#include "content.h"
#include "token.h"

#define EXPORT(name) __attribute__((export_name(name)))

typedef struct {
    uint32_t quality_plus_one, solved, instructions, cycles, ticks;
} score_result;

static uint8_t input[TOKEN_SOLS_MAX];
static score_result result;

EXPORT("score_input") uint8_t *score_input(void) { return input; }
EXPORT("score_capacity") uint32_t score_capacity(void) { return sizeof input; }

EXPORT("score_verify") const score_result *score_verify(uint32_t len)
{
    const int tasks = N_TASKS < CONTENT_N_TASKS ? N_TASKS : CONTENT_N_TASKS;
    if (tasks < 1 || tasks > UINT8_MAX || !len || len > sizeof input)
        return NULL;

    vm_t vm = {0};
    score_result totals = {0};
    uint32_t pos = 0;
    int previous = 0;
    while (pos < len) {
        if (len - pos < 2)
            return NULL;
        int task = input[pos];
        if (task <= previous || task > tasks)
            return NULL;
        previous = task;
        uint8_t code_len = input[pos + 1];
        pos += 2;
        if (code_len == 0 || code_len > len - pos || vm_decode(&vm, input + pos, code_len))
            return NULL;
        pos += code_len;

        const task_def *def = &TASKS[task - 1];
        task_result checked;
        task_run(def, &vm, CONTENT_CHK[task - 1], NULL, &checked);
        if (checked.err[0] || checked.passed != checked.segs)
            return NULL;
        if (def->kind == TASK_NOISE_FLOOR)
            totals.quality_plus_one = (uint32_t)checked.quality + 1;
        totals.solved++;
        totals.instructions += vm.n;
        totals.cycles += checked.cycles;
        totals.ticks += checked.ticks;
    }
    result = totals;
    return &result;
}
