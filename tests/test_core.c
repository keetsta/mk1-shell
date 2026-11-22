/* build/test [content dir]: the chip, the tasks, the font and the shell, against a fake card. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/content.h"
#include "../src/core/font.h"
#include "../src/core/hal.h"
#include "../src/core/sha256.h"
#include "../src/core/shell.h"
#include "../src/core/tasks.h"
#include "../src/core/token.h"
#include "../src/core/vm.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- fake card ---- */
static char outbuf[1 << 16];
static size_t outlen;
static uint16_t matrix[9];
static int matrix_lit, fake_btn, fake_rows, exited;

/* the terminal line the cursor is on, as a person sees it: \r \b \n, CSI K, D, C, J, H */
static char vline[512];
static int vcol;

static void vt(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') {
            vcol = 0;
        } else if (c == '\n') {
            memset(vline, 0, sizeof vline);
            vcol = 0;
        } else if (c == '\b') {
            if (vcol)
                vcol--;
        } else if (c == 0x1b && i + 1 < n && s[i + 1] == '[') {
            int num = 0, has = 0;
            for (i += 2; i < n && s[i] >= '0' && s[i] <= '9'; i++, has = 1)
                num = num * 10 + s[i] - '0';
            if (i < n && s[i] == 'K')
                memset(vline + vcol, 0, sizeof vline - (size_t)vcol);
            else if (i < n && s[i] == 'D')
                vcol = vcol > (has ? num : 1) ? vcol - (has ? num : 1) : 0;
            else if (i < n && s[i] == 'C')
                vcol += has ? num : 1;
            else if (i < n && s[i] == 'J')
                memset(vline, 0, sizeof vline);
            else if (i < n && s[i] == 'H')
                vcol = 0;
        } else if ((unsigned char)c >= 0x20 && vcol < (int)sizeof vline - 1) {
            vline[vcol++] = c;
        }
    }
}

/* the cursor's line; cells never written read as spaces */
static const char *shown(void)
{
    static char b[sizeof vline];
    int last = (int)sizeof vline - 1;
    while (last >= 0 && !vline[last])
        last--;
    for (int i = 0; i <= last; i++)
        b[i] = vline[i] ? vline[i] : ' ';
    b[last + 1] = 0;
    return b;
}

void hal_write(const char *s, size_t n)
{
    vt(s, n);
    if (outlen + n < sizeof outbuf) {
        memcpy(outbuf + outlen, s, n);
        outlen += n;
        outbuf[outlen] = 0;
    }
}
void hal_matrix(const uint16_t r[9])
{
    memcpy(matrix, r, sizeof matrix);
    shell_screen_rows(r);
    for (int i = 0; i < 9; i++)
        if (r[i])
            matrix_lit = 1;
}
int hal_button(void) { return fake_btn; }
uint16_t hal_lux(void) { return 512; }
int hal_rows(void) { return fake_rows; }
void hal_exit(void) { exited = 1; }
/* the state as the card keeps it: one record, loaded only at its exact size */
static uint8_t store[8192];
static size_t store_n;
static int fail_save;
int hal_load(void *buf, size_t n)
{
    if (!store_n || n != store_n)
        return 0;
    memcpy(buf, store, n);
    return 1;
}
int hal_save(const void *buf, size_t n)
{
    if (fail_save || n > HAL_STATE_MAX)
        return -1;
    memcpy(store, buf, n);
    store_n = n;
    return 0;
}

static void type(const char *s)
{
    outlen = 0;
    outbuf[0] = 0;
    for (; *s; s++)
        shell_input(*s);
}
static int said(const char *s) { return strstr(outbuf, s) != NULL; }

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    static char b[8192];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    return b;
}

static void hex(const uint8_t *h, char *out)
{
    for (int i = 0; i < 32; i++)
        sprintf(out + 2 * i, "%02x", h[i]);
}

static vm_t vm;

static int run_src(const char *src, int ticks)
{
    char err[96];
    if (vm_assemble(&vm, src, err, sizeof err))
        return -1;
    vm_reset(&vm, 1);
    for (int i = 0; i < ticks; i++)
        vm_tick(&vm);
    return 0;
}

static int asm_err(const char *src, const char *want)
{
    char err[96] = "";
    return vm_assemble(&vm, src, err, sizeof err) && strstr(err, want);
}

/* ---- the board: hmac, the program as bytes, the token, top ---- */

/* task 1, right but slow to react: 11 instructions */
static const char LAMP[] = "top: mov 0 a\neq btn 1\njf draw\nmov 0xffff a\ndraw: mov 0 row\nn: mov a pix\nadd 1 row\n"
                           "lt row 9\njt n\nwait 10\njmp top\n";
/* made up: the real card's never goes in here */
static const uint8_t test_chipid[8] = {1, 2, 3, 4, 5, 6, 7, 8},
                     test_randid[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static int is_hex(const uint8_t h[32], const char *want)
{
    char s[65];
    hex(h, s);
    return !strcmp(s, want);
}

static void test_hmac(void)
{
    uint8_t key[131], out[32];
    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, "Hi There", 8, out);
    CHECK(is_hex(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, out);
    CHECK(is_hex(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    /* a key longer than a block is hashed first (RFC 4231, test case 6) */
    const char *m = "Test Using Larger Than Block-Size Key - Hash Key First";
    memset(key, 0xaa, sizeof key);
    hmac_sha256(key, sizeof key, m, strlen(m), out);
    CHECK(is_hex(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

static int same_prog(const vm_t *a, const vm_t *b)
{
    if (a->n != b->n)
        return 0;
    for (int i = 0; i < a->n; i++) {
        const vm_ins *x = &a->prog[i], *y = &b->prog[i];
        if (x->op != y->op || x->a.kind != y->a.kind || x->a.v != y->a.v || x->b.kind != y->b.kind ||
            x->b.v != y->b.v)
            return 0;
    }
    return 1;
}

/* what the executor relies on without checking: registers a..d, ports that exist, jumps up to the end */
static int in_bounds(const vm_t *v)
{
    for (int i = 0; i < v->n; i++) {
        const vm_arg *ops[2] = {&v->prog[i].a, &v->prog[i].b};
        for (int j = 0; j < 2; j++) {
            const vm_arg *a = ops[j];
            if (a->kind > K_LABEL || (a->kind == K_REG && a->v > 3) || (a->kind == K_PORT && a->v >= P__N) ||
                (a->kind == K_LABEL && a->v > v->n))
                return 0;
        }
    }
    return v->n <= VM_MAX_INSTR;
}

static uint32_t lcg_next(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void test_code(void)
{
    static vm_t a, b;
    uint8_t code[VM_CODE_MAX], again[VM_CODE_MAX];
    char err[96];
    /* every kind of operand, two numbers in one compare, a label after the last instruction */
    CHECK(vm_assemble(&a, "top: mov 0xffff a\neq 5 700\njf end\nmov btn b\nadd lux b\nnot c\nrev d\ncnt rnd a\n"
                          "mov 3 ea\nmov 7 ed\nmov ed row\nshr 4 pix\nwait clk\njt top\njmp end\nhalt\nend:\n",
                      err, sizeof err) == 0);
    int len = vm_encode(&a, code);
    CHECK(len == 52 && !vm_decode(&b, code, len) && same_prog(&a, &b));
    CHECK(vm_encode(&b, again) == len && !memcmp(code, again, (size_t)len));
    CHECK(b.prog[2].a.kind == K_LABEL && b.prog[2].a.v == b.n);
    uint8_t nops[VM_MAX_INSTR + 1];
    memset(nops, OP_NOP, sizeof nops);
    CHECK(!vm_decode(&b, nops, VM_MAX_INSTR) && b.n == VM_MAX_INSTR && vm_decode(&b, nops, VM_MAX_INSTR + 1));
    /* only what the assembler could have made */
    static const struct { uint8_t len, code[6]; } bad[] = {
        {1, {OP__N}},                    /* no such instruction */
        {3, {OP_MOV, 0x04, 0x00}},       /* register e */
        {3, {OP_MOV, 0x18, 0x00}},       /* port 8 */
        {5, {OP_MOV, 0x20, 0, 1, 0x10}}, /* into btn, which is read-only */
        {5, {OP_MOV, 0x00, 0x20, 0, 1}}, /* into a number */
        {2, {OP_JMP, 2}},                /* past the end */
        {3, {OP_WAIT, 0x20, 0}},         /* a number cut short */
        {2, {OP_MOV, 0x00}},             /* an operand missing */
        {2, {OP_WAIT, 0x21}},            /* no such operand */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        CHECK(vm_decode(&b, bad[i].code, bad[i].len) == -1);
    uint8_t to_end[] = {OP_JMP, 1};
    CHECK(!vm_decode(&b, to_end, 2) && b.n == 1);
    /* bytes from outside, random or a program with a few bytes changed: whatever is taken is what
       vm_encode makes, stays in bounds, and runs */
    uint32_t s = 12345;
    int taken = 0, bad_take = 0;
    for (int round = 0; round < 100000; round++) {
        uint8_t in[64];
        int n;
        if (round & 1) {
            n = len;
            memcpy(in, code, (size_t)n);
            for (int k = 0; k < 3; k++)
                in[lcg_next(&s) % (uint32_t)n] = (uint8_t)lcg_next(&s);
        } else {
            n = (int)(lcg_next(&s) % 40);
            for (int k = 0; k < n; k++)
                in[k] = (uint8_t)lcg_next(&s);
        }
        if (vm_decode(&b, in, n))
            continue;
        taken++;
        if (!in_bounds(&b) || vm_encode(&b, again) != n || memcmp(in, again, (size_t)n))
            bad_take++;
        vm_reset(&b, 7);
        for (int k = 0; k < 20; k++)
            vm_tick(&b);
    }
    CHECK(taken > 1000 && !bad_take);
}

static void test_token(void)
{
    uint8_t key[32], other[32], bin[TOKEN_BIN_MAX], sols[TOKEN_SOLS_MAX];
    char text[TOKEN_TEXT_MAX];
    token_parts t;
    uint16_t n = 0;
    int len;
    token_card_key(test_randid, key);
    CHECK(is_hex(key, "edea6373c65c49344c370445cb3b091eafe000d733d0b936f891bbaf6030fd22"));
    /* one entry a task, in task order, the newest program in place of the old one */
    uint8_t nop = OP_NOP, nop_halt[] = {OP_NOP, OP_HALT};
    CHECK(!token_put(sols, &n, 2, &nop, 1) && !token_put(sols, &n, 1, nop_halt + 1, 1));
    CHECK(!token_put(sols, &n, 1, nop_halt, 2));
    static const uint8_t want[] = {1, 2, OP_NOP, OP_HALT, 2, 1, OP_NOP};
    CHECK(n == sizeof want && !memcmp(sols, want, sizeof want));
    CHECK(token_find(sols, n, 2, &len) == sols + 6 && len == 1 && !token_find(sols, n, 3, &len));
    /* byte for byte what token.h describes, as an independent implementation (Python's hmac and base64)
       made it */
    CHECK(token_make(test_chipid, key, sols, n, text) == 43);
    CHECK(!strcmp(text, "AQECAwQFBgcIAQISEwIBEjgNJk0vwWidoANKpOWPoWc"));
    CHECK(!token_read(text, bin, &t) && !memcmp(t.chipid, test_chipid, 8) && t.sols_len == n &&
          !memcmp(t.sols, sols, n) && !token_check(&t, key));
    /* another card's key, a changed character, a stray bit, a cut: not this card's token */
    uint8_t randid2[16];
    memcpy(randid2, test_randid, 16);
    randid2[15] ^= 1;
    token_card_key(randid2, other);
    CHECK(token_check(&t, other) == -1);
    text[20] = text[20] == 'A' ? 'B' : 'A';
    CHECK(token_read(text, bin, &t) || token_check(&t, key));
    token_make(test_chipid, key, sols, n, text);
    text[42] = 'd';
    CHECK(token_read(text, bin, &t) == -1);
    token_make(test_chipid, key, sols, n, text);
    text[42] = 0;
    CHECK(token_read(text, bin, &t) || token_check(&t, key));
    CHECK(token_read("A", bin, &t) == -1 && token_read("AQECAwQF", bin, &t) == -1 && token_read("AQEC+", bin, &t) == -1);
    /* no room: the entries never outgrow what the state keeps */
    uint8_t big[255] = {0};
    n = 0;
    while (!token_put(sols, &n, n / 257 + 1, big, 255))
        ;
    CHECK(n <= TOKEN_SOLS_MAX && n + 257 > TOKEN_SOLS_MAX && token_entries_ok(sols, n));
}

static void enter_prog(const char *src)
{
    type("edit\r");
    for (const char *p = src; *p; p++)
        shell_input(*p == '\n' ? '\r' : *p);
    type(".\r");
}

/* the link top printed, its token into bin */
static int top_token(uint8_t *bin, token_parts *t)
{
    static char text[TOKEN_TEXT_MAX];
    const char *p = strstr(outbuf, "https://keetsta.me/mk1/top#");
    if (!p)
        return -1;
    p += strlen("https://keetsta.me/mk1/top#");
    size_t n = strcspn(p, "\r\n");
    if (n >= sizeof text)
        return -1;
    memcpy(text, p, n);
    text[n] = 0;
    return token_read(text, bin, t);
}

/* the program kept for task t passes it and has n instructions */
static int kept(const token_parts *tk, int t, int n)
{
    static vm_t v;
    task_result res;
    int len;
    const uint8_t *code = token_find(tk->sols, tk->sols_len, t, &len);
    if (!code || vm_decode(&v, code, len) || v.n != n)
        return 0;
    task_run(&TASKS[t - 1], &v, CONTENT_CHK[t - 1], NULL, &res);
    return res.passed == res.segs;
}

static void test_board(const char *dir)
{
    static uint8_t bin[TOKEN_BIN_MAX];
    static char lamp_nop[sizeof LAMP + 8];
    static const char request[] = "\x1b]7777;nick;aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;c3a978\a";
    static const char reply[] = "\x1b]7777;nick;aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;"
                                "53ed88b9085eb0c566d488f668b6c8e378b1b5e44d01d2618f2899477915865c\a";
    token_parts tk;
    uint8_t key[32];
    char path[512], err[96], *sol2;
    token_card_key(test_randid, key);
    snprintf(lamp_nop, sizeof lamp_nop, "%snop\n", LAMP);
    fake_rows = 0;
    store_n = 0;
    shell_init();
    type("top\r");
    CHECK(said("only a real mk-1 card can go on the board"));
    type("\x1b]7777;hello\a");
    CHECK(!said("\x1b]7777;chip;"));
    type(request);
    CHECK(!outbuf[0]);
    shell_disconnect();
    shell_card(test_chipid, test_randid);
    type(request);
    CHECK(!outbuf[0]);
    type("\x1b]7777;hello\a");
    CHECK(said("\x1b]7777;mk1;2;" MK1_VERSION "\a\x1b]7777;chip;0102030405060708\a"));
    type(request);
    CHECK(!strcmp(outbuf, reply));
    type("\x1b]7777;nick;aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;c3A978\a");
    CHECK(!outbuf[0]);
    type("\x1b]7777;nick;aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;0\a");
    CHECK(!outbuf[0]);
    shell_disconnect();
    type("top\r");
    CHECK(said("solve a task to get on the board"));
    /* the best program is kept: a shorter one replaces it, a longer one does not */
    enter_prog(lamp_nop);
    type("check\r");
    CHECK(said("task 1 passed: 12 instructions"));
    type("top\r");
    CHECK(!top_token(bin, &tk) && !token_check(&tk, key) && kept(&tk, 1, 12));
    int len;
    CHECK(!token_find(tk.sols, tk.sols_len, 2, &len));
    type("task 1\r");
    enter_prog(LAMP);
    type("check\r");
    CHECK(said("task 1 passed: 11 instructions"));
    type("task 1\r");
    enter_prog(lamp_nop);
    type("check\r");
    CHECK(said("task 1 passed: 12 instructions"));
    snprintf(path, sizeof path, "%s/sol2.asm", dir);
    if (!(sol2 = slurp(path)))
        return;
    type("task 2\r");
    enter_prog(sol2);
    type("check\r");
    CHECK(said("task 2 passed"));
    type("top\r");
    CHECK(!top_token(bin, &tk) && !memcmp(tk.chipid, test_chipid, 8) && !token_check(&tk, key));
    CHECK(vm_assemble(&vm, sol2, err, sizeof err) == 0);
    int level_instructions = vm.n;
    CHECK(kept(&tk, 1, 11) && kept(&tk, 2, level_instructions));

    /* a card that played on the firmware before the board: its chapters stay, its programs are asked for */
    struct {
        uint32_t magic;
        uint8_t unlocked, task;
        uint16_t solved;
        uint8_t keys[16][32];
        char prog[2048];
    } old;
    _Static_assert(sizeof old == 2568, "the state the firmware before the board saved");
    task_result res;
    memset(&old, 0, sizeof old);
    old.magic = 0x4d4b3102u;
    old.unlocked = 2;
    old.task = 2;
    old.solved = 1 << 1;
    CHECK(vm_assemble(&vm, LAMP, err, sizeof err) == 0);
    task_run(&TASKS[0], &vm, CONTENT_CHK[0], NULL, &res);
    task_key(res.hash, old.keys[1]);
    strcpy(old.prog, "nop\n");
    memcpy(store, &old, sizeof old);
    store_n = sizeof old;
    shell_init();
    type("tasks\r");
    CHECK(said("1  lamp     solved") && said("> 2  level    open"));
    type("man 1\r");
    CHECK(said("chapter 1. bits"));
    type("list\r");
    CHECK(said(" 1| nop"));
    type("top\r");
    CHECK(said("check task 1 again: the board needs its program"));
    enter_prog(sol2);
    type("check\r");
    type("top\r");
    CHECK(!top_token(bin, &tk) && !token_check(&tk, key) && kept(&tk, 2, level_instructions));
    CHECK(!token_find(tk.sols, tk.sols_len, 1, &len));
    type("task 1\r");
    enter_prog(LAMP);
    type("check\r");
    type("top\r");
    CHECK(!top_token(bin, &tk) && !token_check(&tk, key) && kept(&tk, 1, 11));
    CHECK(store_n > sizeof old && store_n <= HAL_STATE_MAX);
}

int main(int argc, char **argv)
{
    /* sha256 */
    uint8_t h[32];
    char x[65];
    sha256("abc", 3, h);
    hex(h, x);
    CHECK(!strcmp(x, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(m2, strlen(m2), h);
    hex(h, x);
    CHECK(!strcmp(x, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    /* font */
    CHECK(font_check() == 0);

    /* assembler */
    CHECK(asm_err("foo 1 a", "unknown instruction"));
    CHECK(asm_err("mov 1 btn", "read-only"));
    CHECK(asm_err("mov 1 2", "can't write"));
    CHECK(asm_err("jmp nowhere", "no label"));
    CHECK(asm_err("x: nop\nx: nop", "twice"));
    CHECK(asm_err("mov 70000 a", "what is"));
    CHECK(asm_err("add 1", "takes 2 operands"));
    {
        char big[512] = "";
        for (int i = 0; i < VM_MAX_INSTR + 1; i++)
            strcat(big, "nop\n");
        CHECK(asm_err(big, "program memory"));
    }
    CHECK(asm_err("d: nop", "bad label"));
    CHECK(run_src("TOP: MOV 0X10 A ; comment\n# only a comment\n\nb_2: add a,a", 1) == 0 && vm.r[0] == 32);

    /* arithmetic */
    CHECK(run_src("mov 65535 a\nadd 2 a\nmov 0 b\nsub 1 b\nmov -1 c", 1) == 0 && vm.r[0] == 1 && vm.r[1] == 65535 &&
          vm.r[2] == 65535);
    CHECK(run_src("mov 1 a\nshl 16 a\nmov 0x8000 b\nshl 1 b\nmov 0xffff c\nshr 12 c", 1) == 0 && vm.r[0] == 0 &&
          vm.r[1] == 0 && vm.r[2] == 15);
    CHECK(run_src("mov 1 a\nrev a\nmov 0xf0f0 b\ncnt b c", 1) == 0 && vm.r[0] == 0x8000 && vm.r[2] == 8);
    CHECK(run_src("mov 300 a\nmul 300 a\nmov 7 b\ndiv 2 b", 1) == 0 && vm.r[0] == (300 * 300 & 0xffff) && vm.r[1] == 3);
    CHECK(run_src("div 0 a", 1) == 0 && vm.state == VM_ERR && strstr(vm.err, "line 1: division by zero"));
    CHECK(run_src("gt 5 3\njf no\nmov 1 a\nno: lt 5 3\njt end\nmov 1 b\nend:", 1) == 0 && vm.r[0] == 1 &&
          vm.r[1] == 1 && vm.state == VM_HALT);

    /* time */
    CHECK(run_src("mov 1 a\nwait 3\nmov 2 a\nwait 1", 3) == 0 && vm.r[0] == 1);
    vm_tick(&vm);
    CHECK(vm.r[0] == 2 && vm.dev.clk == 4);
    CHECK(run_src("top: jmp top", 1) == 0 && vm.state == VM_ERR && strstr(vm.err, "watchdog"));
    CHECK(run_src("mov clk a\nwait 5\nmov clk b", 6) == 0 && vm.r[0] == 0 && vm.r[1] == 5);

    /* devices */
    CHECK(run_src("mov 3 row\nmov 0b101 pix\nmov 9 row\nmov 0xffff pix\nmov 3 row\nmov pix a", 1) == 0 &&
          vm.dev.screen[3] == 5 && vm.r[0] == 5);
    CHECK(run_src("mov 10 ea\nmov 7 ed\nmov 8 ed\nmov 10 ea\nmov ed a\nmov ed b\nmov ea c", 1) == 0 && vm.r[0] == 7 &&
          vm.r[1] == 8 && vm.r[2] == 12);
    CHECK(run_src("mov rnd a\nmov rnd b", 1) == 0 && vm.r[0] != vm.r[1]);

    /* tasks and chapters */
    const char *dir = argc > 1 ? argv[1] : "content";
    char path[512];
    task_result res;
    for (int t = 1; t <= N_TASKS; t++) {
        snprintf(path, sizeof path, "%s/sol%d.asm", dir, t);
        char *src = slurp(path);
        if (!src) {
            printf("skip: %s not found\n", path);
            continue;
        }
        char err[96];
        CHECK(vm_assemble(&vm, src, err, sizeof err) == 0);
        task_run(&TASKS[t - 1], &vm, CONTENT_CHK[t - 1], NULL, &res);
        CHECK(res.passed == res.segs && !res.err[0]);
        if (t < CONTENT_N_CHAPTERS) {
            uint8_t key[32], txt[64];
            char want[32];
            task_key(res.hash, key);
            chapter_crypt(key, CONTENT_CHAPTERS[t].data, txt, sizeof txt);
            snprintf(want, sizeof want, "chapter %d.", t);
            CHECK(!memcmp(txt, want, strlen(want)));
        }
    }
    /* a program that gets the button backwards fails, and says which test */
    CHECK(vm_assemble(&vm, "top: mov 0xffff a\neq btn 1\njf draw\nmov 0 a\ndraw: mov 0 row\nn: mov a pix\nadd 1 row\n"
                           "lt row 9\njt n\nwait 1\njmp top", x, sizeof x) == 0);
    task_run(&TASKS[0], &vm, CONTENT_CHK[0], NULL, &res);
    CHECK(res.passed < res.segs);
    /* one that is right but slow to react still passes: only the end of each test is sampled */
    CHECK(vm_assemble(&vm, "top: mov 0 a\neq btn 1\njf draw\nmov 0xffff a\ndraw: mov 0 row\nn: mov a pix\nadd 1 row\n"
                           "lt row 9\njt n\nwait 10\njmp top", x, sizeof x) == 0);
    task_run(&TASKS[0], &vm, CONTENT_CHK[0], NULL, &res);
    CHECK(res.passed == res.segs);

    /* shell */
    shell_init();
    type("help\r");
    CHECK(said("man [n]") && said("mk1> "));
    type("nosuch\r");
    CHECK(said("nosuch: no such command"));
    type("man\r");
    CHECK(said("0  chapter 0. the core") && said("locked, solve task 1"));
    type("man 1\r");
    CHECK(said("chapter 1 is locked"));
    type("task 2\r");
    CHECK(said("task 2 is locked"));
    type("try mov 0b1010 pix\r\n");
    CHECK(said("pix=0x000a") && matrix[0] == 10);
    type("try add 5 a\r");
    type("try add 5 a\r");
    CHECK(said("a=10"));
    type("try jmp x\r");
    CHECK(said("only make sense inside a program"));
    type("try mov 1 btn\r");
    CHECK(said("read-only"));
    type("sudo rm -rf /\r");
    CHECK(said("there will be dragons!") && !said("Art by"));
    matrix_lit = 0;
    for (int i = 0; i < 100; i++)
        shell_tick();
    CHECK(matrix_lit);
    for (int i = 0; i < 2000; i++)
        shell_tick();
    CHECK(!matrix[0] && !matrix[4]);
    type("check\r");
    CHECK(said("no program yet"));

    char *sol1 = NULL;
    snprintf(path, sizeof path, "%s/sol1.asm", dir);
    if ((sol1 = slurp(path))) {
        type("edit\r");
        for (char *p = sol1; *p; p++)
            shell_input(*p == '\n' ? '\r' : *p);
        type(".\r");
        CHECK(said("saved, 11 instructions"));
        type("list\r");
        CHECK(said(" 1| top:  mov 0 a") && said("11|       jmp top"));
        type("run\r");
        CHECK(shell_running());
        fake_btn = 1;
        for (int i = 0; i < 3; i++)
            shell_tick();
        CHECK(matrix[0] == 0xffff && matrix[8] == 0xffff);
        type("\x03");
        CHECK(!shell_running() && said("stopped after 3 ticks"));
        type("check\r");
        CHECK(said("task 1 passed") && said("chapter 1 unlocked"));
        type("man 1\r");
        CHECK(said("chapter 1. bits") && said("task 2. level"));
        type("tasks\r");
        CHECK(said("1  lamp     solved") && said("> 2  level    open"));
        type("check\r");
        CHECK(said("test 1 of 10: the outputs are wrong") || said("test 2 of 10: the outputs are wrong") ||
              said("of 10: the outputs are wrong"));
    }
    type("edit\rmov 1 a\r\x03");
    CHECK(said("edit cancelled"));
    type("edit\rdiv 0 a\r.\rrun\r");
    shell_tick();
    CHECK(said("line 1: division by zero") && said("crashed after 1 tick") && !shell_running());
    type("edit\rfoo\r.\r");
    CHECK(said("saved, but: line 1: unknown instruction 'foo'"));

    /* cls replaced clear */
    type("cls\r");
    CHECK(said("\x1b[2J"));
    type("clear\r");
    CHECK(said("clear: no such command"));

    /* history: up and down put earlier commands on the line, redrawn whole with the prompt */
    type("tasks\r");
    type("man\r");
    type("\x1b[A");
    CHECK(!strcmp(shown(), "mk1> man") && vcol == 8);
    type("\x1b[A");
    CHECK(!strcmp(shown(), "mk1> tasks") && vcol == 10);
    type("\x1b[B");
    CHECK(!strcmp(shown(), "mk1> man") && vcol == 8);
    type("\x1b[B");
    CHECK(!strcmp(shown(), "mk1> ") && vcol == 5);
    type("\x1b[B");
    CHECK(!outbuf[0]);
    /* what was being typed survives a trip through the history */
    type("he\x1b[A\x1b[B");
    CHECK(!strcmp(shown(), "mk1> he") && vcol == 7);
    type("lp\r");
    CHECK(said("man [n]"));
    /* application cursor mode sends ESC O A */
    type("\x1bOA\r");
    CHECK(said("man [n]"));
    /* a repeated command is kept once: two steps up from tasks is help */
    type("tasks\rtasks\r\x1b[A\x1b[A\r");
    CHECK(said("man [n]"));
    /* program lines typed in edit stay out of the history */
    type("edit\rmov 1 a\r.\r");
    type("\x1b[A");
    CHECK(!strcmp(shown(), "mk1> edit"));
    /* ^C drops the line and starts again from the newest command */
    type("\x1b[A");
    CHECK(!strcmp(shown(), "mk1> help"));
    type("\x03\x1b[A");
    CHECK(said("^C") && !strcmp(shown(), "mk1> edit"));
    type("\x03");
    /* older commands than HIST_N fall off */
    for (int i = 0; i < 20; i++) {
        char cmd[16];
        snprintf(cmd, sizeof cmd, "man %d\r", i);
        type(cmd);
    }
    for (int i = 0; i < 16; i++)
        type("\x1b[A");
    CHECK(!strcmp(shown(), "mk1> man 4"));
    type("\x1b[A");
    CHECK(!outbuf[0]);
    type("\x03");
    /* random keys never make the screen drift from the buffer: what is on screen, run and recalled
       from the history (a full redraw from the buffer), must be the same line */
    {
        const char *keys[] = {"\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D", "\x1b[H", "\x1b[F", "x", "\x7f", "\x1b[3~",
                              "\x01", "\x05", "\x1bOA", "\x1b[1;5C"};
        uint32_t r = 7;
        for (int round = 0; round < 40; round++) {
            for (int i = 0; i < 40; i++) {
                r = r * 1103515245u + 12345u;
                type(keys[(r >> 16) % 13]);
            }
            type("\x05z");
            char want[160];
            const char *typed = shown() + 5;
            while (*typed == ' ')            /* the history keeps commands without edge spaces */
                typed++;
            snprintf(want, sizeof want, "mk1> %s", typed);
            type("\r\x1b[A");
            CHECK(!strcmp(want, shown()));
            type("\x03");
        }
    }

    /* the wrong keyboard layout: "help" typed as Cyrillic "рудз" never gets into the line, so five
       backspaces can't walk into the prompt and the next "help" runs */
    type("\xd1\x80\xd1\x83\xd0\xb4\xd0\xb7");
    CHECK(!strcmp(outbuf, "\a\a\a\a") && !strcmp(shown(), "mk1> ") && vcol == 5);
    type("\x7f\x7f\x7f\x7f\x7f");
    CHECK(!outbuf[0] && vcol == 5);
    type("help\r");
    CHECK(said("man [n]") && !said("no such command"));
    type("he\xd0\xb7lp\r");                  /* a stray letter in the middle is dropped too */
    CHECK(said("man [n]"));
    type("edit\r\xd0\xb7mov 1 a\r.\r");      /* and in edit */
    CHECK(said("saved, 1 instruction"));

    /* editing in the middle of the line */
    type("hlp\x1b[D\x1b[D");
    CHECK(vcol == 6);
    type("e");
    CHECK(!strcmp(shown(), "mk1> help") && vcol == 7);
    type("\x7f");
    CHECK(!strcmp(shown(), "mk1> hlp") && vcol == 6);
    type("\x1b[3~");
    CHECK(!strcmp(shown(), "mk1> hp") && vcol == 6);
    type("\x1b[H");
    CHECK(vcol == 5);
    type("\x1b[D");
    CHECK(vcol == 5 && !outbuf[0]);
    type("\x1b[F");
    CHECK(vcol == 7);
    type("\x1b[C");
    CHECK(vcol == 7 && !outbuf[0]);
    type("\x01");
    CHECK(vcol == 5);
    type("\x05");
    CHECK(vcol == 7);
    type("\x1b[1~");
    CHECK(vcol == 5);
    type("\x1b[4~");
    CHECK(vcol == 7);
    type("\x1b[1;5D");                       /* ctrl-left: modifiers don't matter */
    CHECK(vcol == 6);
    type("\x1b[D\x03");                      /* ^C in the middle leaves the whole line on screen */
    CHECK(said("hp^C"));
    type("help\x1b[D\x1b[D\r");              /* enter in the middle runs the whole line */
    CHECK(said("man [n]"));
    /* in edit the arrows move the cursor too; up and down do nothing there */
    type("edit\r");
    type("mv 1 a\x1b[H\x1b[Co");
    CHECK(!strcmp(shown(), " 1| mov 1 a") && vcol == 6);
    type("\x1b[A");
    CHECK(!outbuf[0]);
    type("\r.\r");
    CHECK(said("saved, 1 instruction"));
    type("list\r");
    CHECK(said(" 1| mov 1 a"));

    /* ctrl-d leaves at an empty prompt, does nothing on a typed line */
    type("\x04");
    CHECK(exited && said("bye"));
    exited = 0;
    type("man\x04");
    CHECK(!exited);
    type("\x03");

    /* the pager: a screenful, then space a page, enter a line, q stops */
    fake_rows = 10;
    type("man 0\r");
    CHECK(said("chapter 0. the core") && said("-- more: space, enter, q --") && !said("task 1. lamp"));
    {
        int lines = 0;
        for (const char *p = outbuf; (p = strstr(p, "\r\n")); p += 2)
            lines++;
        CHECK(lines == 1 + 9);           /* the echo of enter, then rows - 1 lines */
    }
    type("\x1b[A");                      /* arrows don't page */
    CHECK(!outbuf[0]);
    type("\r");
    CHECK(said("\r                           \r") && said("-- more"));
    {
        char all[8192] = "";
        int guard = 0;
        type(" ");
        while (said("-- more") && guard++ < 50) {
            strncat(all, outbuf, sizeof all - strlen(all) - 1);
            type(" ");
        }
        CHECK(said("task 1. lamp") && said("mk1> "));
    }
    type("man 0\r");
    type("q");
    CHECK(said("mk1> ") && !said("task 1. lamp"));
    type("man 0\r\x03");
    CHECK(said("mk1> "));
    fake_rows = 0;
    type("man 0\r");
    CHECK(said("task 1. lamp") && !said("-- more"));

    /* the web page protocol: hello turns frames on, every change of the screen goes out as an OSC, one
       level 0..f per LED */
    char frame[64 + 144], levels[145];
    memset(levels, '0', 144);
    levels[144] = 0;
    type("try mov 0 pix\r");
    type("\x1b]7777;hello\a");
    snprintf(frame, sizeof frame, "\x1b]7777;f;%s\a", levels);
    CHECK(said("\x1b]7777;mk1;2;" MK1_VERSION "\a") && said(frame));
    type("try mov 0xffff pix\r");
    memset(levels, 'f', 16);
    snprintf(frame, sizeof frame, "\x1b]7777;f;%s\a", levels);
    CHECK(said(frame));
    /* a platform whose screen is more than the shell's matrix (the card's own screens): levels, rounded */
    {
        uint8_t lum[9][16] = {{0}};
        lum[0][0] = 255, lum[0][1] = 77, lum[8][15] = 8;
        type("");
        shell_screen(lum);
        memset(levels, '0', 144);
        levels[0] = 'f', levels[1] = '5';
        snprintf(frame, sizeof frame, "\x1b]7777;f;%s\a", levels);
        CHECK(said(frame));
        type("");
        shell_screen(lum);
        CHECK(!said("\x1b]7777;f;"));
    }
    /* the card has no buzzer, so the chip has no port for one */
    type("try mov 440 bz\r");
    CHECK(said("what is 'bz'?"));
    /* the OSC never reaches the line: typed in the middle of a word, the word still runs */
    type("he\x1b]7777;hello\alp\r");
    CHECK(said("man [n]") && said("\x1b]7777;mk1;2;" MK1_VERSION "\a"));
    /* in a running program too, and ESC \ ends one as well as BEL */
    type("edit\rtop: mov clk a\rwait 1\rjmp top\r.\rrun\r");
    CHECK(shell_running());
    type("\x1b]7777;hello\x1b\\");
    CHECK(shell_running() && said("\x1b]7777;mk1;2;" MK1_VERSION "\a"));
    type("\x03");
    /* other OSCs and overlong ones are dropped without a trace */
    type("\x1b]0;title\a\x1b]7777;helloooooooooooooooooooooooooooooooooooooooooo\a");
    CHECK(!outbuf[0]);
    /* bye stops the frames; so does the card when the terminal goes away */
    type("\x1b]7777;bye\a");
    type("try mov 1 pix\r");
    CHECK(!said("\x1b]7777;f;"));
    type("\x1b]7777;hello\a");
    shell_disconnect();
    type("try mov 3 pix\r");
    CHECK(!said("\x1b]7777;f;"));
    type("he\x1b]7777;unclosed");
    shell_disconnect();
    type("lp\r");
    CHECK(said("man [n]"));
    type("\x1b]7777;unclosed\x1b]7777;hello\a");
    CHECK(said("\x1b]7777;mk1;2;" MK1_VERSION "\a"));
    shell_disconnect();

    fail_save = 1;
    type("edit\rnop\r.\r");
    CHECK(said("could not save progress") && said("program in memory, 1 instruction") && !said("saved,"));
    type("list\r");
    CHECK(said(" 1| nop"));
    fail_save = 0;
    /* arrows still work after all that */
    type("help\r\x1b[A");
    CHECK(!strcmp(shown(), "mk1> help"));
    type("\x03");

    /* a terminal that opens the port late gets the banner and the line as it stands, cursor included */
    type("tsks\x1b[D\x1b[D\x1b[D");
    outlen = 0;
    outbuf[0] = 0;
    shell_connect();
    CHECK(said("mk-1. type help") && !strcmp(shown(), "mk1> tsks"));
    type("a");
    CHECK(!strcmp(shown(), "mk1> tasks"));
    type("\r");
    CHECK(said("1  lamp"));
    type("edit\rtop: wait 1\rjmp top\r.\rrun\r");
    CHECK(shell_running());
    outlen = 0;
    outbuf[0] = 0;
    shell_connect();
    CHECK(said("a program is running, ctrl-c stops") && !said("mk1> "));
    type("\x03");
    CHECK(!shell_running());

    test_hmac();
    test_code();
    test_token();
    test_board(dir);

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
