#include "shell.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "content.h"
#include "font.h"
#include "hal.h"
#include "tasks.h"
#include "token.h"
#include "vm.h"

#define SH_LINE 128
#define HIST_N 16
#define MAX_CHAPTERS 16
#define CHAPTER_MAX 8192
#define STATE_MAGIC 0x4d4b3103u
#define STATE_MAGIC_2 0x4d4b3102u  /* the state before the board, without the programs */
#define MARQUEE_TICKS 3            /* 60 ms per column */

/* everything that survives a power cycle */
typedef struct {
    uint32_t magic;
    uint8_t unlocked;              /* chapters 0..unlocked-1 can be read */
    uint8_t task;                  /* current task, from 1 */
    uint16_t solved;               /* bit t = task t solved */
    uint8_t keys[MAX_CHAPTERS][32];
    char prog[VM_SRC_MAX];
    uint16_t sols_len;
    uint8_t sols[TOKEN_SOLS_MAX];  /* the best program of every solved task, for the board (token.h) */
} saved;

/* the same without the programs; a card that played before keeps its chapters */
typedef struct {
    uint32_t magic;
    uint8_t unlocked;
    uint8_t task;
    uint16_t solved;
    uint8_t keys[MAX_CHAPTERS][32];
    char prog[VM_SRC_MAX];
} saved_2;

_Static_assert(sizeof(saved_2) == 2568 && offsetof(saved, sols_len) == sizeof(saved_2),
               "the old state is the start of the new one, byte for byte");
_Static_assert(sizeof(saved) <= HAL_STATE_MAX, "the state must fit the card's flash sector");

static const char DRAGONS[] = "there will be dragons!";
static const char BANNER[] = "\nmk-1. type help\n";
static const char HEX[] = "0123456789abcdef";

static const char HELP[] =
    "  man [n]     the manual, chapter n\n"
    "  tasks       the tasks and how far you got\n"
    "  task <n>    pick a task\n"
    "  edit        type a program, end it with a line holding only .\n"
    "  list        show the program\n"
    "  run         run it on the card, ctrl-c stops\n"
    "  check       test it against the task\n"
    "  top         a link that puts this card on the board\n"
    "  try <ins>   run one instruction right now\n"
    "  regs        what try has left in the registers\n"
    "  cls         clear the terminal\n"
    "  up, down    earlier commands\n"
    "  ctrl-d      leave\n";

static enum { M_LINE, M_EDIT, M_RUN, M_MORE } mode;
static saved st;
static char line[SH_LINE];
static int len, cur, esc, esc_arg, last_cr;   /* cur: cursor position in line, 0..len */
/* the last HIST_N commands, in RAM only; hist_pos -1 is the line being typed, 0 the newest command */
static char hist[HIST_N][SH_LINE], draft[SH_LINE];
static int hist_n, hist_head, hist_pos = -1;
static char editbuf[VM_SRC_MAX];
static int editlen, editn;
static vm_t vm, bench, checker;
static marquee mq;
static int mq_on, mq_div;
static uint16_t shown[VM_ROWS];
/* the web page (keetsta.me/mk1) protocol, see SPEC.md: an OSC from the page turns web mode on, and then
   every change of what the card's screen shows goes back to it as an OSC the page's terminal hides. The
   platform says what that is (shell_screen): on the card it is not always the shell's matrix */
static int web, osc_st, osc_len;
static char osc[sizeof "7777;nick;" + 2 * TOKEN_NICK_NONCE + 1 + 2 * TOKEN_NICK_MAX];
static uint8_t screen[VM_ROWS * 16]; /* levels 0..15, row 0 first, left to right */
static uint8_t chbuf[CHAPTER_MAX];
static const char *more_p;         /* what the pager has not shown yet */
static uint32_t more_left;
static int card;                   /* the platform gave the card's identity: top can sign */
static uint8_t card_id[8], card_key[32];

/* ---- output -------------------------------------------------------------------- */

static void outn(const char *s, size_t n)
{
    const char *end = s + n;
    while (s < end) {
        const char *nl = memchr(s, '\n', (size_t)(end - s));
        if (!nl) {
            hal_write(s, (size_t)(end - s));
            break;
        }
        hal_write(s, (size_t)(nl - s));
        hal_write("\r\n", 2);
        s = nl + 1;
    }
}

static void out(const char *s) { outn(s, strlen(s)); }

static void outf(const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    out(b);
}

static void prompt(void)
{
    if (mode == M_LINE)
        out("mk1> ");
    else if (mode == M_EDIT)
        outf("%2d| ", editn + 1);
}

/* ESC ] 7777 ; f ; <144 hex digits, one level per LED, row 0 first, left to right> BEL */
static void send_frame(void)
{
    static const char HEAD[] = "\x1b]7777;f;";
    char b[sizeof HEAD - 1 + sizeof screen + 1];
    memcpy(b, HEAD, sizeof HEAD - 1);
    for (size_t i = 0; i < sizeof screen; i++)
        b[sizeof HEAD - 1 + i] = HEX[screen[i]];
    b[sizeof b - 1] = '\a';
    hal_write(b, sizeof b);
}

void shell_screen(const uint8_t lum[VM_ROWS][16])
{
    uint8_t next[sizeof screen];
    for (int y = 0; y < VM_ROWS; y++)
        for (int x = 0; x < 16; x++)
            next[y * 16 + x] = (uint8_t)((lum[y][x] * 15 + 127) / 255);
    if (memcmp(next, screen, sizeof screen)) {
        memcpy(screen, next, sizeof screen);
        if (web)
            send_frame();
    }
}

void shell_screen_rows(const uint16_t rows[VM_ROWS])
{
    uint8_t lum[VM_ROWS][16];
    for (int y = 0; y < VM_ROWS; y++)
        for (int x = 0; x < 16; x++)
            lum[y][x] = rows[y] >> (15 - x) & 1 ? 255 : 0;
    shell_screen(lum);
}

static void show(const uint16_t rows[VM_ROWS])
{
    if (memcmp(rows, shown, sizeof shown)) {
        memcpy(shown, rows, sizeof shown);
        hal_matrix(shown);
    }
}

static int save(void)
{
    int result = hal_save(&st, sizeof st);
    if (result)
        out("could not save progress; changes are only in this session\n");
    return result;
}

/* ---- pager -------------------------------------------------------------------- */

static const char MORE[] = "-- more: space, enter, q --";

/* up to `lines` more lines of the paged text; the pager waits for a key while text is left */
static void more_lines(int lines)
{
    while (more_left && lines-- > 0) {
        const char *nl = memchr(more_p, '\n', more_left);
        uint32_t n = nl ? (uint32_t)(nl - more_p) + 1 : more_left;
        outn(more_p, n);
        more_p += n;
        more_left -= n;
    }
    if (more_left) {
        mode = M_MORE;
        out(MORE);
    } else {
        mode = M_LINE;
    }
}

/* long text goes out a screenful at a time when the terminal can't scroll back (see hal_rows) */
static void page(const char *p, uint32_t n)
{
    int rows = hal_rows();
    more_p = p;
    more_left = n;
    more_lines(rows > 2 ? rows - 1 : 1 << 30);
}

/* space: a page, enter: a line, q or ^C: stop; every other key (arrows too) is ignored */
static void more_key(char c)
{
    int rows = hal_rows(), lines = 0;
    if (c == '\n' && last_cr) {
        last_cr = 0;
        return;
    }
    last_cr = c == '\r';
    if (c == ' ')
        lines = rows > 2 ? rows - 1 : 1;
    else if (c == '\r' || c == '\n')
        lines = 1;
    else if (c == 'q' || c == 3)
        more_left = 0;
    else
        return;
    hal_write("\r", 1);
    for (size_t i = 0; i < sizeof MORE - 1; i++)
        hal_write(" ", 1);
    hal_write("\r", 1);
    more_lines(lines);
    if (mode == M_LINE)
        prompt();
}

/* ---- manual and tasks ------------------------------------------------------------- */

static int chapter_open(int k) { return k < st.unlocked; }
static int task_open(int t) { return t >= 1 && t <= N_TASKS && t <= CONTENT_N_TASKS && t <= st.unlocked; }

/* the readable text of an unlocked chapter */
static const uint8_t *chapter(int k, uint32_t *n)
{
    const content_chapter *c = &CONTENT_CHAPTERS[k];
    *n = c->len;
    if (!c->locked)
        return c->data;
    chapter_crypt(st.keys[k], c->data, chbuf, c->len);
    return chbuf;
}

static int parse_index(const char *s, int *out)
{
    char *e;
    long v = strtol(s, &e, 10);
    if (!*s || *e || v < 0 || v > 99)
        return 0;
    *out = (int)v;
    return 1;
}

static void cmd_man(const char *arg)
{
    int k;
    if (!*arg) {
        for (k = 0; k < CONTENT_N_CHAPTERS; k++) {
            if (!chapter_open(k)) {
                outf("  %d  \xe2\x96\x92\xe2\x96\x92\xe2\x96\x92\xe2\x96\x92  locked, solve task %d\n", k, k);
                continue;
            }
            uint32_t n;
            const uint8_t *t = chapter(k, &n);
            const uint8_t *nl = memchr(t, '\n', n);
            outf("  %d  ", k);
            outn((const char *)t, nl ? (size_t)(nl - t) : n);
            out("\n");
        }
        return;
    }
    if (!parse_index(arg, &k) || k >= CONTENT_N_CHAPTERS) {
        outf("no chapter %s\n", arg);
        return;
    }
    if (!chapter_open(k)) {
        outf("chapter %d is locked: solve task %d\n", k, k);
        return;
    }
    uint32_t n;
    const uint8_t *t = chapter(k, &n);
    page((const char *)t, n);
}

static void cmd_tasks(void)
{
    for (int t = 1; t <= N_TASKS && t <= CONTENT_N_TASKS; t++) {
        const char *mark = t == st.task ? ">" : " ";
        if (!task_open(t))
            outf("%s %d  \xe2\x96\x92\xe2\x96\x92\xe2\x96\x92\xe2\x96\x92\n", mark, t);
        else
            outf("%s %d  %-8s %s\n", mark, t, TASKS[t - 1].name, st.solved >> t & 1 ? "solved" : "open");
    }
}

static void cmd_task(const char *arg)
{
    int t;
    if (!parse_index(arg, &t) || t < 1 || t > N_TASKS || t > CONTENT_N_TASKS) {
        out("usage: task <n>, see tasks\n");
        return;
    }
    if (!task_open(t)) {
        outf("task %d is locked\n", t);
        return;
    }
    st.task = (uint8_t)t;
    save();
    outf("task %d: %s. it is described in man %d\n", t, TASKS[t - 1].name, t - 1);
}

/* the board gets each task's best program so far: the shortest, then the fastest. A stand-in until goals
   with points decide what best means (SPEC.md). The program in `checker` has just passed task t */
static void keep_best(int t, uint32_t cycles)
{
    static uint8_t code[VM_CODE_MAX];
    static vm_t kept;
    static task_result r;
    int old_len;
    const uint8_t *old = token_find(st.sols, st.sols_len, t, &old_len);
    if (old && !vm_decode(&kept, old, old_len)) {
        task_run(&TASKS[t - 1], &kept, CONTENT_CHK[t - 1], NULL, &r);
        if (r.passed == r.segs && (kept.n < checker.n || (kept.n == checker.n && r.cycles <= cycles)))
            return;
    }
    if (token_put(st.sols, &st.sols_len, t, code, vm_encode(&checker, code)))
        out("no room left to keep this program for the board\n");
}

static void cmd_top(void)
{
    static char text[TOKEN_TEXT_MAX];
    if (!card) {
        out("only a real mk-1 card can go on the board\n");
        return;
    }
    if (!st.sols_len) {
        for (int t = 1; t <= N_TASKS && t <= CONTENT_N_TASKS; t++) {
            if (st.solved >> t & 1) {
                outf("check task %d again: the board needs its program, and the old firmware kept none\n", t);
                return;
            }
        }
        out("solve a task to get on the board\n");
        return;
    }
    token_make(card_id, card_key, st.sols, st.sols_len, text);
    out("this card on the board:\nhttps://keetsta.me/mk1/top#");
    out(text);
    out("\n");
}

static void cmd_check(void)
{
    char err[96];
    int t = st.task;
    if (!task_open(t)) {
        out("no task picked, see tasks\n");
        return;
    }
    if (vm_assemble(&checker, st.prog, err, sizeof err)) {
        outf("%s\n", err);
        return;
    }
    if (!checker.n) {
        out("no program yet: edit\n");
        return;
    }
    task_result res;
    task_run(&TASKS[t - 1], &checker, CONTENT_CHK[t - 1], NULL, &res);
    if (res.err[0]) {
        outf("test %d of %d crashed: %s\n", res.passed + 1, res.segs, res.err);
        return;
    }
    if (res.passed < res.segs) {
        outf("test %d of %d: the outputs are wrong\n", res.passed + 1, res.segs);
        return;
    }
    outf("task %d passed: %d instructions, %lu cycles in %lu ticks\n", t, checker.n, (unsigned long)res.cycles,
         (unsigned long)res.ticks);
    st.solved |= (uint16_t)(1u << t);
    keep_best(t, res.cycles);
    if (t < CONTENT_N_CHAPTERS && t < MAX_CHAPTERS && CONTENT_CHAPTERS[t].locked) {
        if (st.unlocked <= t) {
            task_key(res.hash, st.keys[t]);
            st.unlocked = (uint8_t)(t + 1);
            outf("chapter %d unlocked: man %d\n", t, t);
            if (task_open(t + 1))
                st.task = (uint8_t)(t + 1);
        }
    } else {
        out("that was the last chapter so far\n");
    }
    save();
}

/* ---- programs ---------------------------------------------------------------- */

static void cmd_edit(void)
{
    editlen = editn = 0;
    editbuf[0] = 0;
    mode = M_EDIT;
    out("type the program, end it with a line holding only . (ctrl-c keeps the old one)\n");
}

static void edit_line(const char *l)
{
    const char *p = l;
    while (*p == ' ')
        p++;
    if (!strcmp(p, ".")) {
        char err[96];
        memcpy(st.prog, editbuf, (size_t)editlen + 1);
        const char *status = save() ? "program in memory" : "saved";
        mode = M_LINE;
        if (vm_assemble(&vm, st.prog, err, sizeof err))
            outf("%s, but: %s\n", status, err);
        else
            outf("%s, %d instruction%s\n", status, vm.n, vm.n == 1 ? "" : "s");
        return;
    }
    size_t n = strlen(l);
    if (editlen + (int)n + 2 > VM_SRC_MAX) {
        out("the program text is full\n");
        return;
    }
    memcpy(editbuf + editlen, l, n);
    editlen += (int)n;
    editbuf[editlen++] = '\n';
    editbuf[editlen] = 0;
    editn++;
}

static void cmd_list(void)
{
    int n = 0;
    for (const char *s = st.prog; *s;) {
        const char *nl = strchr(s, '\n');
        size_t k = nl ? (size_t)(nl - s) : strlen(s);
        outf("%2d| ", ++n);
        outn(s, k);
        out("\n");
        s += k + (nl ? 1 : 0);
    }
    if (!n)
        out("no program yet: edit\n");
}

static void cmd_run(void)
{
    char err[96];
    if (vm_assemble(&vm, st.prog, err, sizeof err)) {
        outf("%s\n", err);
        return;
    }
    if (!vm.n) {
        out("no program yet: edit\n");
        return;
    }
    vm_reset(&vm, (uint16_t)(hal_lux() * 31 + 7));
    mq_on = 0;
    mode = M_RUN;
    out("running, ctrl-c stops\n");
}

static void run_end(const char *why)
{
    mode = M_LINE;
    outf("%s after %lu tick%s\n", why, (unsigned long)vm.dev.clk, vm.dev.clk == 1 ? "" : "s");
    prompt();
}

static void print_regs(const vm_t *v)
{
    const vm_dev *d = &v->dev;
    outf("a=%u b=%u c=%u d=%u f=%u  row=%u pix=0x%04x ea=%u\n", v->r[0], v->r[1], v->r[2], v->r[3], v->f,
         d->row, d->row < VM_ROWS ? d->screen[d->row] : 0, d->ea);
}

static void cmd_try(const char *arg)
{
    char err[96];
    vm_ins in;
    if (!*arg) {
        out("usage: try <instruction>, for example: try mov 0b1010 pix\n");
        return;
    }
    static const char *const flow[] = {"jmp", "jt", "jf", "wait", "halt"};
    size_t w = strcspn(arg, " ");
    for (int i = 0; i < 5; i++)
        if (w == strlen(flow[i]) && !strncasecmp(arg, flow[i], w)) {
            out("jumps, wait and halt only make sense inside a program\n");
            return;
        }
    if (vm_parse_line(arg, &in, err, sizeof err)) {
        outf("%s\n", err);
        return;
    }
    mq_on = 0;
    bench.dev.btn = hal_button() != 0;
    bench.dev.lux = hal_lux();
    if (vm_exec(&bench, &in))
        outf("%s\n", bench.err);
    show(bench.dev.screen);
    print_regs(&bench);
}

static void cmd_dragon(void)
{
    outf("%s\n", DRAGONS);
    marquee_start(&mq, DRAGONS);
    mq_on = 1;
    mq_div = 0;
}

/* ---- dispatch ------------------------------------------------------------------ */

static void command(char *l)
{
    while (*l == ' ')
        l++;
    char *arg = l;
    while (*arg && *arg != ' ')
        arg++;
    if (*arg)
        *arg++ = 0;
    while (*arg == ' ')
        arg++;
    char *e = arg + strlen(arg);
    while (e > arg && e[-1] == ' ')
        *--e = 0;

    if (!*l)
        return;
    if (!strcmp(l, "help"))
        out(HELP);
    else if (!strcmp(l, "man"))
        cmd_man(arg);
    else if (!strcmp(l, "tasks"))
        cmd_tasks();
    else if (!strcmp(l, "task"))
        cmd_task(arg);
    else if (!strcmp(l, "edit"))
        cmd_edit();
    else if (!strcmp(l, "list"))
        cmd_list();
    else if (!strcmp(l, "run"))
        cmd_run();
    else if (!strcmp(l, "check"))
        cmd_check();
    else if (!strcmp(l, "top"))
        cmd_top();
    else if (!strcmp(l, "try"))
        cmd_try(arg);
    else if (!strcmp(l, "regs"))
        print_regs(&bench);
    else if (!strcmp(l, "cls"))
        out("\x1b[2J\x1b[H");
    else if (!strcmp(l, "objdump") || !strcmp(l, "gdb") || !strcmp(l, "hexdump") || !strcmp(l, "sudo"))
        cmd_dragon();
    else
        outf("%s: no such command, try help\n", l);
}

/* ---- history ------------------------------------------------------------------- */

static char *hist_at(int k) { return hist[(hist_head - 1 - k + HIST_N) % HIST_N]; }

static void hist_add(const char *l)
{
    while (*l == ' ')
        l++;
    size_t n = strlen(l);
    while (n && l[n - 1] == ' ')
        n--;
    if (!n || (hist_n && strlen(hist_at(0)) == n && !strncmp(hist_at(0), l, n)))
        return;
    memcpy(hist[hist_head], l, n);
    hist[hist_head][n] = 0;
    hist_head = (hist_head + 1) % HIST_N;
    if (hist_n < HIST_N)
        hist_n++;
}

/* ---- line editor ----------------------------------------------------------------- */

static void back(int n)
{
    if (n > 0)
        outf("\x1b[%dD", n);
}

/* reprints the line from the cursor on, wipes what the old text left behind, puts the cursor back */
static void redraw_tail(void)
{
    hal_write(line + cur, (size_t)(len - cur));
    out("\x1b[K");
    back(len - cur);
}

/* the whole line again from column 0, prompt included: nothing on screen can drift from the buffer */
static void set_line(const char *s)
{
    size_t n = strlen(s);
    memcpy(line, s, n + 1);
    len = cur = (int)n;
    hal_write("\r", 1);
    prompt();
    hal_write(line, n);
    out("\x1b[K");
}

/* +1 older, -1 newer; walking off the newest end brings back what was being typed */
static void hist_move(int dir)
{
    int to = hist_pos + dir;
    if (to < -1 || to >= hist_n)
        return;
    if (hist_pos == -1) {
        memcpy(draft, line, (size_t)len);
        draft[len] = 0;
    }
    hist_pos = to;
    set_line(to < 0 ? draft : hist_at(to));
}

static void insert_char(char c)
{
    if (len >= SH_LINE - 1)
        return;
    memmove(line + cur + 1, line + cur, (size_t)(len - cur));
    line[cur++] = c;
    len++;
    hal_write(&c, 1);
    if (cur < len)
        redraw_tail();
}

static void delete_at(int at)
{
    memmove(line + at, line + at + 1, (size_t)(len - at - 1));
    len--;
}

static void cursor_end(void)
{
    hal_write(line + cur, (size_t)(len - cur));
    cur = len;
}

/* the final byte of an escape sequence: arrows, home, end, delete; anything else is ignored */
static void key(char k, int arg)
{
    if (k == '~')
        k = arg == 1 || arg == 7 ? 'H' : arg == 4 || arg == 8 ? 'F' : arg == 3 ? 'X' : 0;
    switch (k) {
    case 'A':
    case 'B':
        if (mode == M_LINE)
            hist_move(k == 'A' ? 1 : -1);
        break;
    case 'C':
        if (cur < len)
            hal_write(&line[cur++], 1);
        break;
    case 'D':
        if (cur > 0) {
            cur--;
            hal_write("\b", 1);
        }
        break;
    case 'H':
        back(cur);
        cur = 0;
        break;
    case 'F':
        cursor_end();
        break;
    case 'X':
        if (cur < len) {
            delete_at(cur);
            redraw_tail();
        }
        break;
    }
}

static void key_input(char c)
{
    if (mode == M_RUN) {
        if (c == 3)
            run_end("stopped");
        return;
    }
    if (mode == M_MORE) {
        more_key(c);
        return;
    }
    /* escape sequences: ESC [ <number> [; ...] <final>, or ESC O <final> */
    if (esc) {
        if (esc == 1) {
            esc = c == '[' ? 2 : c == 'O' ? 4 : 0;
            esc_arg = 0;
        } else if (esc == 2 && c >= '0' && c <= '9') {
            if (esc_arg < 1000)
                esc_arg = esc_arg * 10 + (c - '0');
        } else if (c == ';') {
            esc = 3;                 /* further parameters (shift, ctrl) don't matter */
        } else if (c >= 0x40 && c <= 0x7e) {
            esc = 0;
            key(c, esc_arg);
        }
        return;
    }
    if (c == 27) {
        esc = 1;
        return;
    }
    if (c == 4) {            /* ^D on an empty line leaves, like a shell */
        if (mode == M_LINE && !len) {
            out("bye\n");
            hal_exit();
            prompt();
        }
        return;
    }
    if (c == 1) {            /* ^A, ^E: start and end of the line */
        key('H', 0);
        return;
    }
    if (c == 5) {
        key('F', 0);
        return;
    }
    if (c == '\n' && last_cr) {
        last_cr = 0;
        return;
    }
    last_cr = c == '\r';
    if (c == '\r' || c == '\n') {
        cursor_end();
        out("\n");
        line[len] = 0;
        len = cur = 0;
        hist_pos = -1;
        if (mode == M_EDIT) {
            edit_line(line);
        } else {
            hist_add(line);
            command(line);
        }
        prompt();
        return;
    }
    if (c == 3) {
        cursor_end();
        out("^C\n");
        len = cur = 0;
        hist_pos = -1;
        if (mode == M_EDIT) {
            mode = M_LINE;
            out("edit cancelled, the old program stays\n");
        }
        prompt();
        return;
    }
    if (c == 0x7f || c == 8) {
        if (cur > 0) {
            cur--;
            hal_write("\b", 1);
            delete_at(cur);
            redraw_tail();
        }
        return;
    }
    /* the shell speaks ASCII only. A non-ASCII character (Cyrillic from the wrong keyboard layout) is
       several bytes that the terminal draws as one cell: letting them in made backspace and the arrows
       drift from the screen. So it is refused whole, with a bell on its first byte. char is signed on
       the Mac, hence the unsigned look at it */
    unsigned char u = (unsigned char)c;
    if (u >= 0x20 && u < 0x7f)
        insert_char(c);
    else if (u >= 0xc0)
        hal_write("\a", 1);
}

void shell_tick(void)
{
    if (mode == M_RUN) {
        vm.dev.btn = hal_button() != 0;
        vm.dev.lux = hal_lux();
        vm_tick(&vm);
        show(vm.dev.screen);
        if (vm.state == VM_HALT)
            run_end("halted");
        else if (vm.state == VM_ERR) {
            outf("%s\n", vm.err);
            run_end("crashed");
        }
        return;
    }
    if (mq_on && ++mq_div >= MARQUEE_TICKS) {
        uint16_t rows[VM_ROWS];
        mq_div = 0;
        if (marquee_step(&mq, rows)) {
            show(rows);
        } else {
            mq_on = 0;
            memset(rows, 0, sizeof rows);
            show(rows);
        }
    }
}

static void hex_write(char *text, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        text[2 * i] = HEX[data[i] >> 4];
        text[2 * i + 1] = HEX[data[i] & 15];
    }
    text[2 * n] = 0;
}

static int hex_read(uint8_t *data, const char *text, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char *hi = memchr(HEX, text[2 * i], sizeof HEX - 1);
        const char *lo = memchr(HEX, text[2 * i + 1], sizeof HEX - 1);
        if (!hi || !lo)
            return -1;
        data[i] = (uint8_t)((hi - HEX) * 16 + (lo - HEX));
    }
    return 0;
}

static void nick_reply(const char *fields, size_t n)
{
    size_t name_at = 2 * TOKEN_NICK_NONCE + 1;
    if (n <= name_at || n > name_at + 2 * TOKEN_NICK_MAX || (n - name_at) % 2 || fields[name_at - 1] != ';')
        return;
    uint8_t nonce[TOKEN_NICK_NONCE], name[TOKEN_NICK_MAX], proof[32];
    size_t name_len = (n - name_at) / 2;
    if (hex_read(nonce, fields, sizeof nonce) || hex_read(name, fields + name_at, name_len))
        return;
    char proof_hex[2 * sizeof proof + 1];
    if (token_nickname_proof(card_id, card_key, nonce, name, name_len, proof))
        return;
    hex_write(proof_hex, proof, sizeof proof);
    outf("\x1b]7777;nick;%.*s;%s\a", (int)(2 * sizeof nonce), fields, proof_hex);
}

/* an OSC from the page has ended */
static void osc_end(void)
{
    if (osc_len >= (int)sizeof osc || memchr(osc, 0, (size_t)osc_len))
        return;
    osc[osc_len] = 0;
    if (!strcmp(osc, "7777;hello")) {
        web = 1;
        out("\x1b]7777;mk1;2;" MK1_VERSION "\a");
        if (card) {
            char chip[2 * sizeof card_id + 1];
            hex_write(chip, card_id, sizeof card_id);
            outf("\x1b]7777;chip;%s\a", chip);
        }
        send_frame();
    } else if (!strcmp(osc, "7777;bye")) {
        web = 0;
    } else if (web && card && !strncmp(osc, "7777;nick;", sizeof "7777;nick;" - 1)) {
        size_t prefix = sizeof "7777;nick;" - 1;
        nick_reply(osc + prefix, (size_t)osc_len - prefix);
    }
}

/* OSC (ESC ] ... BEL or ESC \) is the page's channel and is taken out of the input in every mode, so it
   never reaches the line, a running program or the pager; any other escape goes on to the keys */
void shell_input(char c)
{
    if (osc_st == 2) {
        if (c == 7) {
            osc_st = 0;
            osc_end();
        } else if (c == 27) {
            osc_st = 3;
        } else if (osc_len < (int)sizeof osc) {
            osc[osc_len++] = c;
        }
        return;
    }
    if (osc_st == 3) {
        osc_st = 0;
        if (c == '\\')
            osc_end();
        else if (c == ']') {
            osc_st = 2;
            osc_len = 0;
        }
        return;
    }
    if (osc_st == 1) {
        osc_st = 0;
        if (c == ']') {
            osc_st = 2;
            osc_len = 0;
            return;
        }
        key_input(27);
        key_input(c);
        return;
    }
    if (c == 27) {
        osc_st = 1;
        return;
    }
    key_input(c);
}

void shell_connect(void)
{
    out(BANNER);
    if (mode == M_RUN) {
        out("a program is running, ctrl-c stops\n");
    } else if (mode == M_MORE) {
        out(MORE);
    } else {
        prompt();
        hal_write(line, (size_t)len);
        back(len - cur);
    }
}

void shell_disconnect(void)
{
    web = osc_st = osc_len = esc = esc_arg = last_cr = 0;
}

int shell_running(void) { return mode == M_RUN; }

void shell_card(const uint8_t chipid[8], const uint8_t randid[16])
{
    memcpy(card_id, chipid, sizeof card_id);
    token_card_key(randid, card_key);
    card = 1;
}

/* the state as it was saved, or the state from before the board with no programs kept yet */
static int load(void)
{
    static saved_2 old;
    if (hal_load(&st, sizeof st) && st.magic == STATE_MAGIC)
        return 1;
    if (!hal_load(&old, sizeof old) || old.magic != STATE_MAGIC_2)
        return 0;
    memset(&st, 0, sizeof st);
    memcpy(&st, &old, sizeof old);
    st.magic = STATE_MAGIC;
    return 1;
}

void shell_init(void)
{
    if (!load() || st.unlocked < 1 || st.unlocked > CONTENT_N_CHAPTERS || st.unlocked > MAX_CHAPTERS) {
        memset(&st, 0, sizeof st);
        st.magic = STATE_MAGIC;
        st.unlocked = 1;
        st.task = 1;
    }
    if (st.sols_len > TOKEN_SOLS_MAX || !token_entries_ok(st.sols, st.sols_len))
        st.sols_len = 0;
    st.prog[VM_SRC_MAX - 1] = 0;
    if (!task_open(st.task))
        st.task = 1;
    vm_reset(&bench, 0x1234);
    bench.state = VM_IDLE;
    mode = M_LINE;
    out(BANNER);
    prompt();
}
