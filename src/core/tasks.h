/* Tasks and the locked manual.
 *
 * A task feeds the program a deterministic input script and samples the nine rows of the matrix at
 * checkpoints. A sample ends in two zero bytes, the place of a buzzer the card no longer has. They
 * are part of every answer, so dropping them would change every chapter key.
 * Samples are chained into a hash:
 *   h0 = 0..0,  h_i = sha256("mk1/smp" | h_{i-1} | sample_i)
 * The firmware keeps only 4 bytes of sha256("mk1/chk" | h_i) per sample: enough to say which sample
 * went wrong, not what it should have been. The final h_n of a correct program is the answer; the next
 * chapter of the manual is encrypted with key = sha256("mk1/key" | h_n). So reading the next chapter
 * means solving the task (or reimplementing the chip, which is also fine).
 */
#ifndef MK1_TASKS_H
#define MK1_TASKS_H

#include <stdint.h>

#include "vm.h"

#define TASK_MAX_SEGS 16
#define TASK_BTN 1     /* the button flips every segment */
#define TASK_LUX 2     /* the light level changes every segment */
#define TASK_EXAMPLES 3
#define TASK_NOISE_PACKETS 128

enum task_kind {
    TASK_LEGACY,
    TASK_FIELD_SYNC,
    TASK_SHADOW_MODEM,
    TASK_MISSING_LINE,
    TASK_NOISE_FLOOR,
};

typedef struct {
    const char *name;
    uint32_t seed;
    uint8_t segs;
    uint8_t vary;
    enum task_kind kind;
} task_def;

typedef struct {
    int segs, passed;          /* passed == segs means solved */
    int wrong_row;             /* first different row, or -1 without a pixel comparison */
    uint32_t ticks, cycles;
    uint16_t quality;           /* noise floor: number of fully decoded noisy packets */
    char err[80];              /* runtime error, if the program crashed */
    uint8_t hash[32];          /* the answer, valid when solved */
} task_result;

extern const task_def TASKS[];
extern const int N_TASKS;

/* Load a public display fixture into U7. Example also shows the uncorrected input frame.
   The index wraps at TASK_EXAMPLES. Return 0 for tasks without display fixtures. */
int task_prepare(const task_def *t, unsigned index, vm_t *vm);
int task_example(const task_def *t, unsigned index, vm_t *vm);

typedef struct {
    uint16_t threshold, duration, tape;
    uint8_t ready, dark;
} task_observer;

/* Call once per tick with a zero-initialized observer to see the clean optical receiver. */
void task_observe(task_observer *state, uint16_t lux, uint16_t rows[VM_ROWS]);

/* Run an assembled program through task t. If supplied, compare the chained sample check bytes
   with expect and write the computed check bytes to chk_out.
   Noise floor reports ticks and cycles from clean qualification and grades the noisy packets separately. */
void task_run(const task_def *t, vm_t *vm, const uint8_t (*expect)[4], uint8_t (*chk_out)[4], task_result *res);

void task_key(const uint8_t answer[32], uint8_t key[32]);
/* XOR with the sha256(key | block counter) stream: encrypts and decrypts */
void chapter_crypt(const uint8_t key[32], const uint8_t *in, uint8_t *out, uint32_t len);

#endif
