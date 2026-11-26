#include "vm.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *const VM_OPS[OP__N] = {
    "mov", "add", "sub", "mul", "div", "and", "or", "xor", "not", "shl", "shr",
    "eq", "lt", "gt", "jmp", "jt", "jf", "wait", "nop", "halt", "rev", "cnt",
};
/* operands: s = value (register, port or number), d = destination (register or writable port), l = label */
static const char *const SIG[OP__N] = {
    "sd", "sd", "sd", "sd", "sd", "sd", "sd", "sd", "d", "sd", "sd",
    "ss", "ss", "ss", "l", "l", "l", "s", "", "", "d", "sd",
};
const char *const VM_PORTS[P__N] = {"btn", "lux", "clk", "rnd", "row", "pix", "ea", "ed"};
static const uint8_t PORT_WRITABLE[P__N] = {0, 0, 0, 0, 1, 1, 1, 1};

#define MAX_TOK 4
#define LABEL_LEN 16
#define ASM_LINE_MAX 127

typedef struct { char name[LABEL_LEN]; uint8_t at; } label;

/* ---- parsing ------------------------------------------------------------------- */

static int is_ident(const char *s)
{
    if (!(isalpha((unsigned char)*s) || *s == '_'))
        return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_'))
            return 0;
    return 1;
}

static int find_reg(const char *s) { return s[0] >= 'a' && s[0] <= 'd' && !s[1] ? s[0] - 'a' : -1; }

static int find_port(const char *s)
{
    for (int i = 0; i < P__N; i++)
        if (!strcmp(s, VM_PORTS[i]))
            return i;
    return -1;
}

static int find_op(const char *s)
{
    for (int i = 0; i < OP__N; i++)
        if (!strcmp(s, VM_OPS[i]))
            return i;
    return -1;
}

typedef enum { NUM_OK, NUM_NOT_LITERAL, NUM_INVALID, NUM_RANGE } num_result;

static num_result parse_num(const char *s, uint16_t *out)
{
    int neg = *s == '-', base = 10;
    long limit = neg ? 32768 : 65535;
    long v = 0;
    if (!isdigit((unsigned char)*s) && !neg && *s != '+')
        return NUM_NOT_LITERAL;
    if (neg)
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'b')) {
        base = s[1] == 'x' ? 16 : 2;
        s += 2;
    }
    if (!*s)
        return NUM_INVALID;
    for (; *s; s++) {
        int d;
        if (isdigit((unsigned char)*s))
            d = *s - '0';
        else if (*s >= 'a' && *s <= 'f')
            d = *s - 'a' + 10;
        else
            return NUM_INVALID;
        if (d >= base)
            return NUM_INVALID;
        if (v <= limit)
            v = v * base + d;
    }
    if (v > limit)
        return NUM_RANGE;
    *out = (uint16_t)(neg ? -v : v);
    return NUM_OK;
}

/* lowercases, drops the comment, splits off "label:" and the operands */
static int split(const char *src, size_t n, char buf[ASM_LINE_MAX + 1], char **lab, char **tok,
                 char *err, size_t errlen)
{
    while (n && (*src == ' ' || *src == '\t')) {
        src++;
        n--;
    }
    size_t end = 0;
    while (end < n && src[end] != '#' && src[end] != ';')
        end++;
    while (end && strchr(" \t,\r", src[end - 1]))
        end--;
    if (end > ASM_LINE_MAX) {
        snprintf(err, errlen, "line too long (max %d characters of code)", ASM_LINE_MAX);
        return -1;
    }
    for (size_t i = 0; i < end; i++)
        buf[i] = (char)tolower((unsigned char)src[i]);
    buf[end] = 0;
    *lab = NULL;
    char *body = buf;
    char *colon = strchr(buf, ':');
    if (colon) {
        *colon = 0;
        char *l = buf;
        while (*l == ' ' || *l == '\t')
            l++;
        char *e = l + strlen(l);
        while (e > l && (e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        *lab = l;
        body = colon + 1;
    }
    int nt = 0;
    for (char *p = strtok(body, " \t,\r"); p; p = strtok(NULL, " \t,\r")) {
        if (nt == MAX_TOK) {
            snprintf(err, errlen, "too many operands");
            return -1;
        }
        tok[nt++] = p;
    }
    return nt;
}

static int parse_ins(char **tok, int nt, const label *labs, int nlabs, vm_ins *out, char *err, size_t errlen)
{
    int op = find_op(tok[0]);
    if (op < 0) {
        snprintf(err, errlen, "unknown instruction '%s'", tok[0]);
        return -1;
    }
    const char *sig = SIG[op];
    if (!labs && (sig[0] == 'l' || op == OP_WAIT || op == OP_HALT)) {
        snprintf(err, errlen, "jumps, wait and halt only make sense inside a program");
        return -1;
    }
    int want = (int)strlen(sig);
    if (nt - 1 != want) {
        snprintf(err, errlen, "'%s' takes %d operand%s", tok[0], want, want == 1 ? "" : "s");
        return -1;
    }
    memset(out, 0, sizeof *out);
    out->op = (uint8_t)op;
    for (int i = 0; i < want; i++) {
        const char *t = tok[i + 1];
        vm_arg *a = i ? &out->b : &out->a;
        int r, p;
        if (sig[i] == 'l') {
            int found = -1;
            for (int j = 0; j < nlabs; j++)
                if (!strcmp(labs[j].name, t))
                    found = j;
            if (found < 0) {
                snprintf(err, errlen, "no label '%s'", t);
                return -1;
            }
            a->kind = K_LABEL;
            a->v = labs[found].at;
        } else if ((r = find_reg(t)) >= 0) {
            a->kind = K_REG;
            a->v = (uint16_t)r;
        } else if ((p = find_port(t)) >= 0) {
            if (sig[i] == 'd' && !PORT_WRITABLE[p]) {
                snprintf(err, errlen, "'%s' is read-only", t);
                return -1;
            }
            a->kind = K_PORT;
            a->v = (uint16_t)p;
        } else {
            switch (parse_num(t, &a->v)) {
            case NUM_NOT_LITERAL:
                snprintf(err, errlen, "what is '%s'?", t);
                return -1;
            case NUM_INVALID:
                snprintf(err, errlen, "invalid number '%s'", t);
                return -1;
            case NUM_RANGE:
                snprintf(err, errlen, "number out of range (-32768 to 65535): '%s'", t);
                return -1;
            case NUM_OK:
                break;
            }
            if (sig[i] == 'd') {
                snprintf(err, errlen, "can't write to a number");
                return -1;
            }
            a->kind = K_LIT;
        }
    }
    return 0;
}

static const char *next_line(const char *s, size_t *len)
{
    const char *e = strchr(s, '\n');
    *len = e ? (size_t)(e - s) : strlen(s);
    return e ? e + 1 : s + *len;
}

int vm_assemble(vm_t *vm, const char *src, char *err, size_t errlen)
{
    label labs[VM_MAX_INSTR + 1];
    int nlabs = 0, n = 0, lineno = 0;
    char buf[ASM_LINE_MAX + 1], *lab, *tok[MAX_TOK], e[64];

    vm->n = 0;
    vm->state = VM_IDLE;
    /* pass 1: count instructions, place labels */
    for (const char *s = src; *s;) {
        size_t len;
        const char *line = s;
        s = next_line(s, &len);
        lineno++;
        int nt = split(line, len, buf, &lab, tok, e, sizeof e);
        if (nt < 0) {
            snprintf(err, errlen, "line %d: %s", lineno, e);
            return -1;
        }
        if (lab) {
            if (!is_ident(lab) || strlen(lab) >= LABEL_LEN || find_reg(lab) >= 0 || find_port(lab) >= 0 ||
                find_op(lab) >= 0) {
                snprintf(err, errlen, "line %d: bad label '%s'", lineno, lab);
                return -1;
            }
            for (int j = 0; j < nlabs; j++)
                if (!strcmp(labs[j].name, lab)) {
                    snprintf(err, errlen, "line %d: label '%s' twice", lineno, lab);
                    return -1;
                }
            if (nlabs == VM_MAX_INSTR + 1) {
                snprintf(err, errlen, "line %d: too many labels", lineno);
                return -1;
            }
            strcpy(labs[nlabs].name, lab);
            labs[nlabs++].at = (uint8_t)n;
        }
        if (nt > 0 && ++n > VM_MAX_INSTR) {
            snprintf(err, errlen, "line %d: program memory is %d instructions", lineno, VM_MAX_INSTR);
            return -1;
        }
    }
    /* pass 2: instructions */
    lineno = 0;
    n = 0;
    for (const char *s = src; *s;) {
        size_t len;
        const char *line = s;
        s = next_line(s, &len);
        lineno++;
        int nt = split(line, len, buf, &lab, tok, e, sizeof e);
        if (nt == 0)
            continue;
        if (parse_ins(tok, nt, labs, nlabs, &vm->prog[n], e, sizeof e)) {
            snprintf(err, errlen, "line %d: %s", lineno, e);
            return -1;
        }
        vm->prog[n++].line = (uint16_t)lineno;
    }
    vm->n = (uint8_t)n;
    return 0;
}

int vm_parse_line(const char *line, vm_ins *out, char *err, size_t errlen)
{
    char buf[ASM_LINE_MAX + 1], *lab, *tok[MAX_TOK];
    int nt = split(line, strlen(line), buf, &lab, tok, err, errlen);
    if (nt < 0)
        return -1;
    if (lab) {
        snprintf(err, errlen, "labels only work in programs");
        return -1;
    }
    if (nt == 0) {
        snprintf(err, errlen, "nothing to run");
        return -1;
    }
    return parse_ins(tok, nt, NULL, 0, out, err, errlen);
}

/* ---- execution ----------------------------------------------------------------- */

void vm_reset(vm_t *vm, uint16_t seed)
{
    memset(vm->r, 0, sizeof vm->r);
    vm->f = vm->pc = 0;
    vm->sleep = 0;
    vm->cycles = 0;
    vm->err[0] = 0;
    vm->state = VM_RUN;
    memset(vm->dev.screen, 0, sizeof vm->dev.screen);
    vm->dev.row = vm->dev.clk = 0;
    vm->dev.ea = 0;
    vm->dev.rnd = seed ? seed : 0xace1;
}

static uint16_t port_read(vm_dev *d, int p, int peek)
{
    switch (p) {
    case P_BTN: return d->btn;
    case P_LUX: return d->lux;
    case P_CLK: return d->clk;
    case P_RND:
        if (peek)
            return d->rnd;
        d->rnd ^= (uint16_t)(d->rnd << 7);
        d->rnd ^= (uint16_t)(d->rnd >> 9);
        d->rnd ^= (uint16_t)(d->rnd << 8);
        return d->rnd;
    case P_ROW: return d->row;
    case P_PIX: return d->row < VM_ROWS ? d->screen[d->row] : 0;
    case P_EA: return d->ea;
    case P_ED: {
        uint16_t v = d->ee[d->ea];
        if (!peek)
            d->ea++;
        return v;
    }
    }
    return 0;
}

static void port_write(vm_dev *d, int p, uint16_t v)
{
    switch (p) {
    case P_ROW: d->row = v; break;
    case P_PIX:
        if (d->row < VM_ROWS)
            d->screen[d->row] = v;
        break;
    case P_EA: d->ea = (uint8_t)v; break;
    case P_ED: d->ee[d->ea++] = (uint8_t)v; break;
    }
}

/* peek: the destination of add/sub/... is read without side effects */
static uint16_t rd(vm_t *vm, const vm_arg *a, int peek)
{
    switch (a->kind) {
    case K_LIT: return a->v;
    case K_REG: return vm->r[a->v];
    case K_PORT: return port_read(&vm->dev, a->v, peek);
    }
    return 0;
}

static void wr(vm_t *vm, const vm_arg *a, uint16_t v)
{
    if (a->kind == K_REG)
        vm->r[a->v] = v;
    else if (a->kind == K_PORT)
        port_write(&vm->dev, a->v, v);
}

static int fail(vm_t *vm, const vm_ins *in, const char *what)
{
    if (in->line)
        snprintf(vm->err, sizeof vm->err, "line %d: %s", in->line, what);
    else
        snprintf(vm->err, sizeof vm->err, "%s", what);
    vm->state = VM_ERR;
    return -1;
}

static uint16_t rev16(uint16_t v)
{
    uint16_t r = 0;
    for (int i = 0; i < 16; i++, v >>= 1)
        r = (uint16_t)(r << 1 | (v & 1));
    return r;
}

/* 0 go on, 1 tick handed back, 2 halted, -1 error */
static int exec(vm_t *vm, const vm_ins *in, uint8_t *pc)
{
    const vm_arg *a = &in->a, *b = &in->b;
    uint16_t s, d;
    switch (in->op) {
    case OP_MOV: wr(vm, b, rd(vm, a, 0)); break;
    case OP_ADD: s = rd(vm, a, 0); wr(vm, b, (uint16_t)(rd(vm, b, 1) + s)); break;
    case OP_SUB: s = rd(vm, a, 0); wr(vm, b, (uint16_t)(rd(vm, b, 1) - s)); break;
    case OP_MUL: s = rd(vm, a, 0); wr(vm, b, (uint16_t)((uint32_t)rd(vm, b, 1) * s)); break;
    case OP_DIV:
        s = rd(vm, a, 0);
        if (!s)
            return fail(vm, in, "division by zero");
        wr(vm, b, rd(vm, b, 1) / s);
        break;
    case OP_AND: s = rd(vm, a, 0); wr(vm, b, rd(vm, b, 1) & s); break;
    case OP_OR: s = rd(vm, a, 0); wr(vm, b, rd(vm, b, 1) | s); break;
    case OP_XOR: s = rd(vm, a, 0); wr(vm, b, rd(vm, b, 1) ^ s); break;
    case OP_NOT: wr(vm, a, (uint16_t)~rd(vm, a, 1)); break;
    case OP_SHL: s = rd(vm, a, 0); d = rd(vm, b, 1); wr(vm, b, s >= 16 ? 0 : (uint16_t)(d << s)); break;
    case OP_SHR: s = rd(vm, a, 0); d = rd(vm, b, 1); wr(vm, b, s >= 16 ? 0 : (uint16_t)(d >> s)); break;
    case OP_EQ: s = rd(vm, a, 0); vm->f = s == rd(vm, b, 0); break;
    case OP_LT: s = rd(vm, a, 0); vm->f = s < rd(vm, b, 0); break;
    case OP_GT: s = rd(vm, a, 0); vm->f = s > rd(vm, b, 0); break;
    case OP_JMP: *pc = (uint8_t)a->v; break;
    case OP_JT: if (vm->f) *pc = (uint8_t)a->v; break;
    case OP_JF: if (!vm->f) *pc = (uint8_t)a->v; break;
    case OP_WAIT:
        s = rd(vm, a, 0);
        vm->sleep = s ? s : 1;
        return 1;
    case OP_NOP: break;
    case OP_HALT: vm->state = VM_HALT; return 2;
    case OP_REV: wr(vm, a, rev16(rd(vm, a, 1))); break;
    case OP_CNT: {
        uint16_t v = rd(vm, a, 0), c = 0;
        for (; v; v &= (uint16_t)(v - 1))
            c++;
        wr(vm, b, c);
        break;
    }
    }
    return 0;
}

int vm_exec(vm_t *vm, const vm_ins *in)
{
    uint8_t pc = 0;
    vm->err[0] = 0;
    int r = exec(vm, in, &pc);
    vm->cycles++;
    if (vm->state == VM_ERR)
        vm->state = VM_IDLE;
    return r < 0 ? -1 : 0;
}

void vm_tick(vm_t *vm)
{
    if (vm->state != VM_RUN)
        return;
    if (vm->sleep && --vm->sleep)
        goto done;
    for (int k = 0;; k++) {
        if (vm->pc >= vm->n) {
            vm->state = VM_HALT;
            break;
        }
        if (k == VM_TICK_BUDGET) {
            char m[64];
            snprintf(m, sizeof m, "watchdog: %d instructions without wait", VM_TICK_BUDGET);
            fail(vm, &vm->prog[vm->pc], m);
            break;
        }
        const vm_ins *in = &vm->prog[vm->pc];
        uint8_t pc = (uint8_t)(vm->pc + 1);
        int r = exec(vm, in, &pc);
        vm->cycles++;
        if (r >= 0)
            vm->pc = pc;
        if (r)
            break;
    }
done:
    vm->dev.clk++;
}

/* ---- code ---------------------------------------------------------------------- */

static int put_arg(uint8_t *out, int k, const vm_arg *a)
{
    switch (a->kind) {
    case K_REG:
    case K_LABEL: out[k++] = (uint8_t)a->v; break;
    case K_PORT: out[k++] = (uint8_t)(0x10 + a->v); break;
    case K_LIT:
        out[k++] = 0x20;
        out[k++] = (uint8_t)(a->v >> 8);
        out[k++] = (uint8_t)a->v;
        break;
    }
    return k;
}

int vm_encode(const vm_t *vm, uint8_t out[VM_CODE_MAX])
{
    int k = 0;
    for (int i = 0; i < vm->n; i++) {
        const vm_ins *in = &vm->prog[i];
        const char *sig = SIG[in->op];
        out[k++] = in->op;
        if (sig[0])
            k = put_arg(out, k, &in->a);
        if (sig[0] && sig[1])
            k = put_arg(out, k, &in->b);
    }
    return k;
}

/* the executor indexes registers and ports with these values unchecked: only what parse_ins allows passes */
static int get_arg(const uint8_t *code, int len, int *k, char sig, vm_arg *a)
{
    if (*k >= len)
        return -1;
    uint8_t b = code[(*k)++];
    if (sig == 'l') {
        a->kind = K_LABEL;   /* the target is checked once the length of the program is known */
        a->v = b;
    } else if (b < 4) {
        a->kind = K_REG;
        a->v = b;
    } else if (b >= 0x10 && b < 0x10 + P__N && (sig == 's' || PORT_WRITABLE[b - 0x10])) {
        a->kind = K_PORT;
        a->v = (uint16_t)(b - 0x10);
    } else if (b == 0x20 && sig == 's' && *k + 2 <= len) {
        a->kind = K_LIT;
        a->v = (uint16_t)(code[*k] << 8 | code[*k + 1]);
        *k += 2;
    } else {
        return -1;
    }
    return 0;
}

int vm_decode(vm_t *vm, const uint8_t *code, int len)
{
    int n = 0, k = 0;
    vm->n = 0;
    vm->state = VM_IDLE;
    while (k < len) {
        if (n == VM_MAX_INSTR || code[k] >= OP__N)
            return -1;
        vm_ins *in = &vm->prog[n++];
        memset(in, 0, sizeof *in);
        in->op = code[k++];
        const char *sig = SIG[in->op];
        if (sig[0] && get_arg(code, len, &k, sig[0], &in->a))
            return -1;
        if (sig[0] && sig[1] && get_arg(code, len, &k, sig[1], &in->b))
            return -1;
    }
    /* a label after the last instruction is the program's end: jumping there halts it */
    for (int i = 0; i < n; i++)
        if (vm->prog[i].a.kind == K_LABEL && vm->prog[i].a.v > n)
            return -1;
    vm->n = (uint8_t)n;
    return 0;
}
