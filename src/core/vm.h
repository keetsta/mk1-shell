/* MK-1: the made-up chip the card's shell is programmed in.
 *
 * 16-bit words, four registers a b c d, a test flag, ports for the card's real devices.
 * A screen row is one word: bit 15 is the leftmost of the 16 columns.
 * Time runs in ticks of 20 ms; a program runs until `wait` hands the tick back.
 */
#ifndef MK1_VM_H
#define MK1_VM_H

#include <stddef.h>
#include <stdint.h>

#define VM_ROWS 9
#define VM_MAX_INSTR 24        /* program memory, in instructions */
#define VM_TICK_BUDGET 128     /* instructions per tick before the watchdog fires */
#define VM_SRC_MAX 2048
#define VM_TICK_MS 20

enum {
    OP_MOV, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_AND, OP_OR, OP_XOR, OP_NOT, OP_SHL, OP_SHR,
    OP_EQ, OP_LT, OP_GT, OP_JMP, OP_JT, OP_JF, OP_WAIT, OP_NOP, OP_HALT, OP_REV, OP_CNT,
    OP__N
};

enum { K_NONE, K_LIT, K_REG, K_PORT, K_LABEL };

enum { P_BTN, P_LUX, P_CLK, P_RND, P_ROW, P_PIX, P_EA, P_ED, P__N };

enum { VM_IDLE, VM_RUN, VM_HALT, VM_ERR };

typedef struct { uint8_t kind; uint16_t v; } vm_arg;
typedef struct { uint8_t op; vm_arg a, b; uint16_t line; } vm_ins;

/* what the program sees of the card; the platform (or a task) fills the inputs every tick */
typedef struct {
    uint16_t screen[VM_ROWS];
    uint16_t row, clk, rnd, lux;
    uint8_t btn, ea;
    uint8_t ee[256];           /* U7, the black box */
} vm_dev;

typedef struct {
    vm_ins prog[VM_MAX_INSTR];
    uint8_t n;
    uint16_t r[4];
    uint8_t f, pc, state;
    uint16_t sleep;
    uint32_t cycles;
    char err[80];
    vm_dev dev;
} vm_t;

extern const char *const VM_OPS[OP__N];
extern const char *const VM_PORTS[P__N];

/* 0 on success, otherwise err holds "line N: ..." and the old program is gone */
int vm_assemble(vm_t *vm, const char *src, char *err, size_t errlen);
/* one non-flow instruction with no labels, for `try`; 0 on success */
int vm_parse_line(const char *line, vm_ins *out, char *err, size_t errlen);
/* registers, flag and outputs back to zero, keeps the program and the EEPROM */
void vm_reset(vm_t *vm, uint16_t seed);
/* one 20 ms tick */
void vm_tick(vm_t *vm);
/* executes a single non-flow instruction outside a program; 0 ok, -1 error in vm->err */
int vm_exec(vm_t *vm, const vm_ins *in);

/* The program as bytes, to keep it and to send it to the board (token.h): per instruction the opcode, then
 * each operand: a register 0x00..0x03, a port 0x10 + n, a number 0x20 and two bytes high first, a jump
 * target as the instruction's index */
#define VM_CODE_MAX (VM_MAX_INSTR * 7)   /* every instruction comparing two numbers */
int vm_encode(const vm_t *vm, uint8_t out[VM_CODE_MAX]);
/* code from outside (a token): accepts exactly the bytes vm_encode makes of an assembled program, so what
   it loads is safe to run. 0 ok, -1 not a program */
int vm_decode(vm_t *vm, const uint8_t *code, int len);

#endif
