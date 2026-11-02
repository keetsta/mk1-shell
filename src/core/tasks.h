/* Tasks and the locked manual.
 *
 * A task feeds the program a script of inputs in segments (button held or not, a light level) and
 * samples the outputs (9 screen rows and the buzzer) at the end of each segment, so the program has the
 * whole segment to settle and exact timing does not matter. Samples are chained into a hash:
 *   h0 = 0..0,  h_i = sha256("mc1/smp" | h_{i-1} | sample_i)
 * The firmware keeps only 4 bytes of sha256("mc1/chk" | h_i) per sample: enough to say which sample
 * went wrong, not what it should have been. The final h_n of a correct program is the answer; the next
 * chapter of the manual is encrypted with key = sha256("mc1/key" | h_n). So reading the next chapter
 * means solving the task (or reimplementing the chip, which is also fine).
 */
#ifndef MC1_TASKS_H
#define MC1_TASKS_H

#include <stdint.h>

#include "vm.h"

#define TASK_MAX_SEGS 16
#define TASK_BTN 1     /* the button flips every segment */
#define TASK_LUX 2     /* the light level changes every segment */

typedef struct {
    const char *name;
    uint32_t seed;
    uint8_t segs;
    uint8_t vary;
} task_def;

typedef struct {
    int segs, passed;          /* passed == segs means solved */
    uint32_t ticks, cycles;
    char err[80];              /* runtime error, if the program crashed */
    uint8_t hash[32];          /* the answer, valid when solved */
} task_result;

extern const task_def TASKS[];
extern const int N_TASKS;

/* runs an assembled program through task t; expect = per-sample check bytes or NULL (just compute);
   chk_out, if not NULL, receives the check bytes of every sample */
void task_run(const task_def *t, vm_t *vm, const uint8_t (*expect)[4], uint8_t (*chk_out)[4], task_result *res);

void task_key(const uint8_t answer[32], uint8_t key[32]);
/* XOR with the sha256(key | block counter) stream: encrypts and decrypts */
void chapter_crypt(const uint8_t key[32], const uint8_t *in, uint8_t *out, uint32_t len);

#endif
