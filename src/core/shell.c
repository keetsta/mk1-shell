#include "shell.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "content.h"
#include "font.h"
#include "hal.h"
#include "tasks.h"
#include "vm.h"

#define SH_LINE 128
#define HIST_N 16
#define MAX_CHAPTERS 16
#define CHAPTER_MAX 8192
#define STATE_MAGIC 0x4d433102u
#define MARQUEE_TICKS 3            /* 60 ms per column */

/* everything that survives a power cycle */
typedef struct {
    uint32_t magic;
    uint8_t unlocked;              /* chapters 0..unlocked-1 can be read */
    uint8_t task;                  /* current task, from 1 */
    uint16_t solved;               /* bit t = task t solved */
    uint8_t keys[MAX_CHAPTERS][32];
    char prog[VM_SRC_MAX];
} saved;

/* In the dump in plain text, and printed by the commands people reach for first.
 * The dragon is ASCII art by Jcro...@vt.edu (its signature lives here, not on screen),
 * the lettering is figlet's "standard" font with kerning. */
static const char DRAGON[] =
    "         \\`----.__                 ____\n"
    "          |       `--._         <=#  , *--,\n"
    "         /_             `-.    ,/  / `````\n"
    "           \\__             (_.'  ,'\n"
    "              \\__ ......'       \\___----^__\n"
    "           _./               ,'           `.\n"
    "  |\\     _.'   ___/ )\\...._\"   ___           \\\n"
    "  | \\__.'  __.'            `\"\"'   `\"\"`.'\"\"\"`--\\\n"
    "   \\____.-'\n"
    "\n"
    "   _    _                                   _  _  _   _\n"
    "  | |_ | |__    ___  _ __  ___  __      __ (_)| || | | |__    ___\n"
    "  | __|| '_ \\  / _ \\| '__|/ _ \\ \\ \\ /\\ / / | || || | | '_ \\  / _ \\\n"
    "  | |_ | | | ||  __/| |  |  __/  \\ V  V /  | || || | | |_) ||  __/\n"
    "   \\__||_| |_| \\___||_|   \\___|   \\_/\\_/   |_||_||_| |_.__/  \\___|\n"
    "\n"
    "       _                                        _\n"
    "    __| | _ __  __ _   __ _   ___   _ __   ___ | |\n"
    "   / _` || '__|/ _` | / _` | / _ \\ | '_ \\ / __|| |\n"
    "  | (_| || |  | (_| || (_| || (_) || | | |\\__ \\|_|\n"
    "   \\__,_||_|   \\__,_| \\__, | \\___/ |_| |_||___/(_)\n"
    "                      |___/\n";
static const char DRAGONS[] = "there will be dragons!";

static const char HELP[] =
    "  man [n]     the manual, chapter n\n"
    "  tasks       the tasks and how far you got\n"
    "  task <n>    pick a task\n"
    "  edit        type a program, end it with a line holding only .\n"
    "  list        show the program\n"
    "  run         run it on the card, ctrl-c stops\n"
    "  check       test it against the task\n"
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
static uint16_t shown[VM_ROWS], tone_hz;
static uint8_t chbuf[CHAPTER_MAX];
static const char *more_p;         /* what the pager has not shown yet */
static uint32_t more_left;

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
        out("mc1> ");
    else if (mode == M_EDIT)
        outf("%2d| ", editn + 1);
}

static void show(const uint16_t rows[VM_ROWS])
{
    if (memcmp(rows, shown, sizeof shown)) {
        memcpy(shown, rows, sizeof shown);
        hal_matrix(shown);
    }
}

static void tone(uint16_t hz)
{
    if (hz != tone_hz)
        hal_tone(tone_hz = hz);
}

static void save(void) { hal_save(&st, sizeof st); }

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
        save();
        mode = M_LINE;
        if (vm_assemble(&vm, st.prog, err, sizeof err))
            outf("saved, but: %s\n", err);
        else
            outf("saved, %d instruction%s\n", vm.n, vm.n == 1 ? "" : "s");
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
    tone(0);
    mode = M_LINE;
    outf("%s after %lu tick%s\n", why, (unsigned long)vm.dev.clk, vm.dev.clk == 1 ? "" : "s");
    prompt();
}

static void print_regs(const vm_t *v)
{
    const vm_dev *d = &v->dev;
    outf("a=%u b=%u c=%u d=%u f=%u  row=%u pix=0x%04x bz=%u ea=%u\n", v->r[0], v->r[1], v->r[2], v->r[3], v->f,
         d->row, d->row < VM_ROWS ? d->screen[d->row] : 0, d->bz, d->ea);
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
    tone(bench.dev.bz);
    print_regs(&bench);
}

static void cmd_dragon(void)
{
    out(DRAGON);
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

void shell_input(char c)
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
        tone(vm.dev.bz);
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

int shell_running(void) { return mode == M_RUN; }

void shell_init(void)
{
    if (!hal_load(&st, sizeof st) || st.magic != STATE_MAGIC || st.unlocked < 1 ||
        st.unlocked > CONTENT_N_CHAPTERS || st.unlocked > MAX_CHAPTERS) {
        memset(&st, 0, sizeof st);
        st.magic = STATE_MAGIC;
        st.unlocked = 1;
        st.task = 1;
    }
    st.prog[VM_SRC_MAX - 1] = 0;
    if (!task_open(st.task))
        st.task = 1;
    vm_reset(&bench, 0x1234);
    bench.state = VM_IDLE;
    mode = M_LINE;
    out("\nmc-1. type help\n");
    prompt();
}
