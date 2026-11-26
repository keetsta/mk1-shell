#include "shell.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    "  edit        open the program editor (ctrl-o saves, ctrl-x exits)\n"
    "  edit new    start with an empty editor\n"
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
static int len, cur, line_full, last_cr;   /* cur: cursor position in line, 0..len */
static int esc, esc_arg, esc_second;
/* the last HIST_N commands, in RAM only; hist_pos -1 is the line being typed, 0 the newest command */
static char hist[HIST_N][SH_LINE], draft[SH_LINE];
static int hist_n, hist_head, hist_pos = -1;
static char editbuf[VM_SRC_MAX];
static char edit_message[128];
static int editlen, editcur, edit_goal, edit_top, edit_left, edit_redraw;
static int edit_asking, edit_paste, edit_dot, program_in_memory;
static int term_rows, term_cols, term_probe;
static int reported_rows, reported_cols;
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
    else
        program_in_memory = 0;
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

static void select_task(int t)
{
    st.task = (uint8_t)t;
    outf("selected task %d: %s (man %d)\n", t, TASKS[t - 1].name, t - 1);
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
    select_task(t);
    save();
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
        outf("inputs: btn=%u lux=%u\n", checker.dev.btn, checker.dev.lux);
        if (t == 2)
            out("rows start at 0. row 4 is the middle row (fifth from the top).\n"
                "try mov 4 row, then try mov 0x8000 pix to see it.\n");
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
                select_task(t + 1);
        }
    } else {
        out("that was the last chapter so far\n");
    }
    save();
}

/* ---- programs ---------------------------------------------------------------- */

static int edit_bol(int at)
{
    while (at && editbuf[at - 1] != '\n')
        at--;
    return at;
}

static int edit_eol(int at)
{
    while (at < editlen && editbuf[at] != '\n')
        at++;
    return at;
}

static int edit_dirty(void) { return strcmp(editbuf, st.prog) != 0; }

static int edit_rows(void) { return term_rows ? term_rows : reported_rows ? reported_rows : 12; }
static int edit_cols(void) { return term_cols ? term_cols : reported_cols ? reported_cols : 40; }
static const char *edit_status(void) { return edit_asking ? "save changes? y/n" : edit_message; }

static int edit_body_rows(void)
{
    int rows = edit_rows(), width = edit_cols() - 1;
    int status_rows = ((int)strlen(edit_status()) + width - 1) / width;
    int limit = rows >= 8 ? 3 : rows >= 5 ? 2 : 1;
    if (status_rows < 1)
        status_rows = 1;
    if (status_rows > limit)
        status_rows = limit;
    int body = rows - 2 - status_rows;
    return body > 0 ? body : 1;
}

static void edit_screen_line(int row, const char *text, int width)
{
    outf("\x1b[%d;1H\x1b[K", row);
    if (width)
        hal_write(text, (size_t)width);
}

static int edit_field(int row, const char *text, int width)
{
    int n = (int)strlen(text);
    if (n > width)
        n = width;
    edit_screen_line(row, text, n);
    return n;
}

static void edit_draw(void)
{
    edit_redraw = 0;
    if (!shell_editing() || term_rows < 0)
        return;

    int rows = edit_rows(), width = edit_cols() - 1;
    int header = rows > 3, body = edit_body_rows(), prefix = width >= 12 ? 5 : 0;
    int lineno = 0, col = editcur - edit_bol(editcur);
    for (int i = 0; i < editcur; i++)
        lineno += editbuf[i] == '\n';
    if (lineno < edit_top)
        edit_top = lineno;
    if (lineno >= edit_top + body)
        edit_top = lineno - body + 1;
    if (col < edit_left)
        edit_left = col;
    if (col >= edit_left + width - prefix)
        edit_left = col - (width - prefix) + 1;

    char b[160];
    if (header) {
        snprintf(b, sizeof b, "mk-1 edit%s", edit_dirty() ? " *" : "");
        edit_field(1, b, width);
    }
    int at = 0;
    for (int i = 0; i < edit_top; i++)
        at = edit_eol(at) + 1;
    for (int i = 0; i < body; i++) {
        int n = 0;
        if (at <= editlen) {
            if (prefix)
                n = snprintf(b, sizeof b, "%4d ", edit_top + i + 1);
            int end = edit_eol(at);
            for (int j = at + edit_left; j < end && n < width; j++) {
                unsigned char c = (unsigned char)editbuf[j];
                b[n++] = c == '\t' ? ' ' : c >= 0x20 && c < 0x7f ? (char)c : '?';
            }
            at = end + 1;
        }
        edit_screen_line(header + i + 1, b, n);
    }
    const char *message = edit_status();
    for (int row = header + body + 1; row < rows; row++)
        message += edit_field(row, message, width);
    if (rows >= 2)
        edit_field(rows, edit_asking ? "^c back" : "^o save  ^x exit", width);
    outf("\x1b[%d;%dH", header + lineno - edit_top + 1, prefix + col - edit_left + 1);
}

static void edit_probe(void)
{
    if (!term_rows) {
        term_probe = 1;
        out("\x1b[999;999H\x1b[6n");
    }
}

static void edit_screen_open(void)
{
    if (term_rows < 0)
        out("ctrl-o saves, ctrl-x exits, a final . saves and exits\n");
    else {
        out("\x1b[?1049h\x1b[?2004h\x1b[2J");
        edit_probe();
        edit_draw();
    }
}

static void cmd_edit(const char *arg)
{
    if (*arg && strcmp(arg, "new")) {
        out("usage: edit [new]\n");
        return;
    }
    strcpy(editbuf, *arg ? "" : st.prog);
    editlen = (int)strlen(editbuf);
    editcur = edit_top = edit_left = edit_asking = edit_paste = edit_dot = 0;
    edit_goal = -1;
    edit_message[0] = 0;
    mode = M_EDIT;
    edit_screen_open();
}

static void edit_leave(void)
{
    if (term_rows >= 0)
        out("\x1b[?2004l\x1b[?1049l");
    term_probe = 0;
    mode = M_LINE;
}

static void edit_close(const char *message)
{
    edit_leave();
    if (*message)
        outf("%s\n", message);
    prompt();
}

static void edit_save(int close)
{
    char err[96];
    memcpy(st.prog, editbuf, (size_t)editlen + 1);
    if (close)
        edit_leave();
    int failed = close ? save() : hal_save(&st, sizeof st);
    program_in_memory = failed;
    const char *status = failed ? "program in memory" : "saved";
    if (vm_assemble(&vm, st.prog, err, sizeof err)) {
        snprintf(edit_message, sizeof edit_message, "%s, but: %s", status, err);
        if (!close && !strncmp(err, "line ", 5)) {
            long lineno = strtol(err + 5, NULL, 10);
            editcur = 0;
            for (long i = 1; i < lineno && edit_eol(editcur) < editlen; i++)
                editcur = edit_eol(editcur) + 1;
            edit_goal = -1;
        }
    } else {
        snprintf(edit_message, sizeof edit_message, "%s, %d instruction%s", status, vm.n, vm.n == 1 ? "" : "s");
    }
    if (close || term_rows < 0)
        outf("%s\n", edit_message);
    if (close)
        prompt();
}

static void edit_insert(const char *s, int n)
{
    if (editlen + n >= VM_SRC_MAX) {
        snprintf(edit_message, sizeof edit_message, "program text is full (%d bytes)", VM_SRC_MAX - 1);
        if (term_rows < 0)
            outf("%s\n", edit_message);
        return;
    }
    memmove(editbuf + editcur + n, editbuf + editcur, (size_t)(editlen - editcur + 1));
    memcpy(editbuf + editcur, s, (size_t)n);
    editcur += n;
    editlen += n;
    edit_message[0] = 0;
}

static void edit_delete(void)
{
    if (editcur < editlen) {
        memmove(editbuf + editcur, editbuf + editcur + 1, (size_t)(editlen - editcur));
        editlen--;
        edit_message[0] = 0;
    }
}

static void edit_move_lines(int count)
{
    int at = edit_bol(editcur);
    if (edit_goal < 0)
        edit_goal = editcur - at;
    while (count < 0 && at) {
        at = edit_bol(at - 1);
        count++;
    }
    while (count > 0 && edit_eol(at) < editlen) {
        at = edit_eol(at) + 1;
        count--;
    }
    int end = edit_eol(at);
    editcur = at + edit_goal < end ? at + edit_goal : end;
}

static void edit_key(char k)
{
    if (edit_asking || edit_paste)
        return;
    edit_dot = 0;
    if (k == 'A' || k == 'B' || k == 'P' || k == 'N') {
        int count = k == 'A' || k == 'B' ? 1 : edit_body_rows();
        edit_move_lines(k == 'A' || k == 'P' ? -count : count);
    } else {
        edit_goal = -1;
        switch (k) {
        case 'C': if (editcur < editlen) editcur++; break;
        case 'D': if (editcur > 0) editcur--; break;
        case 'H': editcur = edit_bol(editcur); break;
        case 'F': editcur = edit_eol(editcur); break;
        case 'X': edit_delete(); break;
        }
    }
    edit_redraw = 1;
}

static void edit_input(char c)
{
    int dot = edit_dot;
    edit_dot = 0;
    if (edit_asking) {
        if (c == 'y' || c == 'Y')
            edit_save(1);
        else if (c == 'n' || c == 'N')
            edit_close(program_in_memory ? "edit cancelled. previous program is only in this session" : "edit cancelled");
        else if (c == 3)
            edit_asking = 0;
    } else if (!edit_paste && (c == 24 || c == 3)) {
        if (!edit_dirty())
            edit_close(program_in_memory ? "program is only in this session" : "");
        else {
            edit_asking = 1;
            if (term_rows < 0)
                out("save changes? y/n\n");
        }
    } else if (!edit_paste && (c == 15 || c == 19)) {
        edit_save(0);
    } else if (!edit_paste && c == 12) {
        edit_probe();
    } else if (!edit_paste && (c == 1 || c == 5)) {
        edit_key(c == 1 ? 'H' : 'F');
    } else if (!edit_paste && (c == 8 || c == 0x7f)) {
        if (editcur > 0) {
            editcur--;
            edit_delete();
        }
        edit_goal = -1;
    } else if (c == '\r' || c == '\n') {
        /* The old dot terminator still works for line-by-line input. Pasted dots are literal. */
        int start = edit_bol(editcur);
        const char *p = editbuf + start;
        while (*p == ' ')
            p++;
        if (dot && !edit_paste && editcur == editlen && !strcmp(p, ".")) {
            editbuf[start] = 0;
            editlen = start;
            edit_save(1);
        } else {
            edit_insert("\n", 1);
            edit_goal = -1;
        }
    } else if (c == '\t') {
        edit_insert("    ", 4 - (editcur - edit_bol(editcur)) % 4);
        edit_goal = -1;
    } else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7f) {
        int before = editlen;
        edit_insert(&c, 1);
        edit_dot = !edit_paste && c == '.' && editlen > before;
        edit_goal = -1;
    } else if ((unsigned char)c >= 0xc0 && !edit_paste) {
        out("\a");
    }
    edit_redraw = 1;
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
    if (bench.dev.row >= VM_ROWS)
        outf("row %u is outside 0..%d (0 is the top). pix has no effect there\n", bench.dev.row, VM_ROWS - 1);
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
        cmd_edit(arg);
    else if (!strcmp(l, "list"))
        cmd_list();
    else if (!strcmp(l, "run"))
        cmd_run();
    else if (!strcmp(l, "check")) {
        if (*arg)
            out("usage: check (use task <n> to select a task)\n");
        else
            cmd_check();
    }
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
    if (len >= SH_LINE - 1) {
        if (!line_full)
            out("\a");
        line_full = 1;
        return;
    }
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
    if (mode == M_EDIT && k == '~') {
        if (arg == 200 || arg == 201) {
            edit_paste = arg == 200 && !edit_asking;
            edit_dot = 0;
            return;
        }
        if (arg == 5 || arg == 6) {
            edit_key(arg == 5 ? 'P' : 'N');
            return;
        }
    }
    if (k == '~')
        k = arg == 1 || arg == 7 ? 'H' : arg == 4 || arg == 8 ? 'F' : arg == 3 ? 'X' : 0;
    if (mode == M_EDIT) {
        edit_key(k);
        return;
    }
    switch (k) {
    case 'A':
    case 'B':
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
            esc_arg = esc_second = 0;
        } else if ((esc == 2 || esc == 3) && c >= '0' && c <= '9') {
            int *arg = esc == 2 ? &esc_arg : &esc_second;
            if (*arg < 1000)
                *arg = *arg * 10 + (c - '0');
        } else if (c == ';') {
            esc = esc == 2 ? 3 : 5;
        } else if (c >= 0x40 && c <= 0x7e) {
            if (esc == 3 && c == 'R' && mode == M_EDIT && term_probe && esc_arg && esc_second) {
                reported_rows = esc_arg > 100 ? 100 : esc_arg;
                reported_cols = esc_second > 160 ? 160 : esc_second < 2 ? 2 : esc_second;
                term_probe = 0;
                edit_redraw = 1;
            } else {
                key(c, esc_arg);
            }
            esc = 0;
        }
        return;
    }
    if (c == 27) {
        esc = 1;
        return;
    }
    if (c == '\n' && last_cr) {
        last_cr = 0;
        return;
    }
    last_cr = c == '\r';
    if (mode == M_EDIT) {
        edit_input(c);
        return;
    }
    if (c == 4) {            /* ^D on an empty line leaves, like a shell */
        if (!len) {
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
    if (c == '\r' || c == '\n') {
        cursor_end();
        out("\n");
        line[len] = 0;
        len = cur = 0;
        hist_pos = -1;
        if (line_full) {
            line_full = 0;
            out("command is too long\n");
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
        len = cur = line_full = 0;
        hist_pos = -1;
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
    if (mode == M_EDIT && edit_redraw)
        edit_draw();
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
    if (mode == M_EDIT) {
        edit_screen_open();
        return;
    }
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
    web = osc_st = osc_len = esc = esc_arg = esc_second = last_cr = 0;
    edit_dot = edit_paste = term_probe = reported_rows = reported_cols = 0;
}

int shell_running(void) { return mode == M_RUN; }
int shell_editing(void) { return mode == M_EDIT; }

void shell_resize(int rows, int cols)
{
    term_rows = rows > 100 ? 100 : rows;
    term_cols = cols > 160 ? 160 : cols == 1 ? 2 : cols;
    edit_redraw = 1;
}

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
    program_in_memory = 0;
    mode = M_LINE;
    out(BANNER);
    prompt();
}
