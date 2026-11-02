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
    for (int i = 0; i < 9; i++)
        if (r[i])
            matrix_lit = 1;
}
void hal_tone(uint16_t hz) { (void)hz; }
int hal_button(void) { return fake_btn; }
uint16_t hal_lux(void) { return 512; }
int hal_rows(void) { return fake_rows; }
void hal_exit(void) { exited = 1; }
int hal_load(void *buf, size_t n) { (void)buf; (void)n; return 0; }
void hal_save(const void *buf, size_t n) { (void)buf; (void)n; }

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
    CHECK(said("man [n]") && said("mc1> "));
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
    CHECK(said("<=#  , *--,") && said("\\__,_||_|   \\__,_|") && said("|___/") && !said("Art by"));
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
    CHECK(!strcmp(shown(), "mc1> man") && vcol == 8);
    type("\x1b[A");
    CHECK(!strcmp(shown(), "mc1> tasks") && vcol == 10);
    type("\x1b[B");
    CHECK(!strcmp(shown(), "mc1> man") && vcol == 8);
    type("\x1b[B");
    CHECK(!strcmp(shown(), "mc1> ") && vcol == 5);
    type("\x1b[B");
    CHECK(!outbuf[0]);
    /* what was being typed survives a trip through the history */
    type("he\x1b[A\x1b[B");
    CHECK(!strcmp(shown(), "mc1> he") && vcol == 7);
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
    CHECK(!strcmp(shown(), "mc1> edit"));
    /* ^C drops the line and starts again from the newest command */
    type("\x1b[A");
    CHECK(!strcmp(shown(), "mc1> help"));
    type("\x03\x1b[A");
    CHECK(said("^C") && !strcmp(shown(), "mc1> edit"));
    type("\x03");
    /* older commands than HIST_N fall off */
    for (int i = 0; i < 20; i++) {
        char cmd[16];
        snprintf(cmd, sizeof cmd, "man %d\r", i);
        type(cmd);
    }
    for (int i = 0; i < 16; i++)
        type("\x1b[A");
    CHECK(!strcmp(shown(), "mc1> man 4"));
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
            snprintf(want, sizeof want, "mc1> %s", typed);
            type("\r\x1b[A");
            CHECK(!strcmp(want, shown()));
            type("\x03");
        }
    }

    /* the wrong keyboard layout: "help" typed as Cyrillic "рудз" never gets into the line, so five
       backspaces can't walk into the prompt and the next "help" runs */
    type("\xd1\x80\xd1\x83\xd0\xb4\xd0\xb7");
    CHECK(!strcmp(outbuf, "\a\a\a\a") && !strcmp(shown(), "mc1> ") && vcol == 5);
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
    CHECK(!strcmp(shown(), "mc1> help") && vcol == 7);
    type("\x7f");
    CHECK(!strcmp(shown(), "mc1> hlp") && vcol == 6);
    type("\x1b[3~");
    CHECK(!strcmp(shown(), "mc1> hp") && vcol == 6);
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
        CHECK(said("task 1. lamp") && said("mc1> "));
    }
    type("man 0\r");
    type("q");
    CHECK(said("mc1> ") && !said("task 1. lamp"));
    type("man 0\r\x03");
    CHECK(said("mc1> "));
    fake_rows = 0;
    type("man 0\r");
    CHECK(said("task 1. lamp") && !said("-- more"));

    /* the web page protocol: hello turns frames on, every change of the matrix goes out as an OSC */
    type("try mov 0 pix\r");
    type("\x1b]7777;hello\a");
    CHECK(said("\x1b]7777;mc1;1;" MC1_VERSION "\a") && said("\x1b]7777;f;000000000000000000000000000000000000;0\a"));
    type("try mov 0xffff pix\r");
    CHECK(said("\x1b]7777;f;ffff00000000000000000000000000000000;0\a"));
    type("try mov 440 bz\r");
    CHECK(said(";440\a"));
    type("try mov 0 bz\r");
    /* the OSC never reaches the line: typed in the middle of a word, the word still runs */
    type("he\x1b]7777;hello\alp\r");
    CHECK(said("man [n]") && said("\x1b]7777;mc1;1;" MC1_VERSION "\a"));
    /* in a running program too, and ESC \ ends one as well as BEL */
    type("edit\rtop: mov clk a\rwait 1\rjmp top\r.\rrun\r");
    CHECK(shell_running());
    type("\x1b]7777;hello\x1b\\");
    CHECK(shell_running() && said("\x1b]7777;mc1;1;" MC1_VERSION "\a"));
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
    /* arrows still work after all that */
    type("help\r\x1b[A");
    CHECK(!strcmp(shown(), "mc1> help"));
    type("\x03");

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
