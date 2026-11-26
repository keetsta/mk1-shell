/* The card in a Mac terminal: the matrix is drawn in the top lines, the shell scrolls underneath.
 * While a program runs, space toggles the button and + / - change the light level. ^D at an empty
 * prompt quits. State is kept in $MK1_STATE or ~/.mk1-shell.
 *
 * The shell scrolls inside a scroll region below the panel. Terminals drop the region on resize, and a
 * few ignore it, so the region is set again on SIGWINCH and the panel is redrawn after every output
 * that moved lines. The region is not saved to scrollback, so the manual is paged (hal_rows).
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "../core/hal.h"
#include "../core/shell.h"
#include "../core/vm.h"

#define TOP 12                      /* first line of the shell area */

static struct termios saved_tio;
static int tty, dirty = 1, btn, lines = 24, cols = 80, top = TOP, layout_top, was_editing;
static volatile sig_atomic_t resized;
static uint16_t rows[VM_ROWS], lux = 512;
static char state_path[1024];

/* Setting a scroll region homes the cursor. Keep it where the shell left it. */
static void layout(void)
{
    if (lines > 1)
        printf("\x1b" "7\x1b[%d;%dr\x1b" "8", top, lines);
    if (layout_top != top)
        printf("\x1b[%d;1H\x1b[K", top);
    layout_top = top;
    dirty = 1;
}

void hal_write(const char *s, size_t n)
{
    int editing = shell_editing();
    if (tty && was_editing && !editing)
        layout();
    was_editing = editing;
    fwrite(s, 1, n, stdout);
    if (!tty || editing)
        return;
    /* `cls` wipes the matrix area too: put it back */
    if (n >= 4 && memmem(s, n, "\x1b[2J", 4)) {
        printf("\x1b[%d;1H", top);
        dirty = 1;
    }
    if (memchr(s, '\n', n))
        dirty = 1;
}

void hal_matrix(const uint16_t r[VM_ROWS])
{
    memcpy(rows, r, sizeof rows);
    dirty = 1;
    shell_screen_rows(r);
}

int hal_button(void) { return btn; }
uint16_t hal_lux(void) { return lux; }
int hal_rows(void) { return tty ? lines - top + 1 : 0; }
void hal_exit(void) { exit(0); }

int hal_load(void *buf, size_t n)
{
    FILE *f = fopen(state_path, "rb");
    if (!f)
        return 0;
    size_t k = fread(buf, 1, n, f);
    int ok = k == n && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    return ok;
}

int hal_save(const void *buf, size_t n)
{
    char temp[sizeof state_path + sizeof ".tmp.XXXXXX"];
    snprintf(temp, sizeof temp, "%s.tmp.XXXXXX", state_path);
    int fd = mkstemp(temp);
    if (fd < 0)
        return -1;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(temp);
        return -1;
    }
    int ok = fwrite(buf, 1, n, f) == n && !fflush(f) && !fsync(fd);
    if (fclose(f))
        ok = 0;
    if (ok && !rename(temp, state_path))
        return 0;
    unlink(temp);
    return -1;
}

static void terminal_size(void)
{
    struct winsize ws;
    if (!ioctl(1, TIOCGWINSZ, &ws)) {
        if (ws.ws_row)
            lines = ws.ws_row;
        if (ws.ws_col)
            cols = ws.ws_col;
    }
    top = lines >= TOP + 3 && cols >= 34 ? TOP : 1;
    shell_resize(lines, cols);
}

/* whole lines are wiped, so text that scrolled under the panel (a terminal without the region) is hidden */
static void draw(void)
{
    printf("\x1b" "7\x1b[1;1H\x1b[2K");
    for (int r = 0; r < VM_ROWS; r++) {
        printf("\x1b[%d;1H\x1b[2K  ", r + 2);
        for (int c = 0; c < 16; c++)
            fputs(rows[r] >> (15 - c) & 1 ? "\x1b[38;5;202m\xe2\x97\x8f " : "\x1b[38;5;238m\xc2\xb7 ", stdout);
        fputs("\x1b[0m", stdout);
    }
    printf("\x1b[%d;1H\x1b[2K    btn %d   lux %4u   %s", VM_ROWS + 2, btn, lux,
           shell_running() ? "space = button, + - = light" : "");
    printf("\x1b" "8");
    dirty = 0;
}

static void on_winch(int sig)
{
    (void)sig;
    resized = 1;
}

static void restore(void)
{
    if (!tty)
        return;
    if (shell_editing())
        printf("\x1b[?2004l\x1b[?1049l");
    printf("\x1b[r\x1b[%d;1H\r\n", lines);
    fflush(stdout);
    tcsetattr(0, TCSANOW, &saved_tio);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
    const char *p = getenv("MK1_STATE");
    const char *dir = getenv("HOME");
    int path_len = p ? snprintf(state_path, sizeof state_path, "%s", p)
                     : snprintf(state_path, sizeof state_path, "%s/.mk1-shell", dir ? dir : ".");
    if (path_len < 0 || (size_t)path_len >= sizeof state_path) {
        fputs("state path is too long\n", stderr);
        return 1;
    }

    tty = isatty(0) && isatty(1);
    if (tty) {
        struct termios t;
        tcgetattr(0, &saved_tio);
        t = saved_tio;
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
        t.c_iflag &= ~(tcflag_t)(ICRNL | IXON);
        t.c_oflag &= ~(tcflag_t)OPOST;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        terminal_size();
        printf("\x1b[2J");
        layout();
        printf("\x1b[%d;1H", top);
        atexit(restore);
        signal(SIGWINCH, on_winch);
    } else
        shell_resize(-1, 0);

    shell_init();
    long long next = now_ms() + VM_TICK_MS;
    for (;;) {
        if (tty && resized) {
            resized = 0;
            terminal_size();
            if (!shell_editing()) {
                int panel_changed = layout_top != top;
                layout();
                if (panel_changed)
                    shell_connect();
            }
        }
        if (tty && dirty && top != 1 && !shell_editing())
            draw();
        fflush(stdout);
        long long wait = next - now_ms();
        if (wait < 0)
            wait = 0;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(0, &fds);
        struct timeval tv = {0, (int)wait * 1000};
        int r = select(1, &fds, NULL, NULL, &tv);
        if (r < 0 && errno != EINTR)
            break;
        if (r > 0) {
            char buf[256];
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0 && !tty)
                break;
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (shell_running() && (c == ' ' || c == '+' || c == '=' || c == '-')) {
                    if (c == ' ')
                        btn ^= 1;
                    else if (c == '-')
                        lux = lux >= 64 ? (uint16_t)(lux - 64) : 0;
                    else
                        lux = lux <= 1023 - 64 ? (uint16_t)(lux + 64) : 1023;
                    dirty = 1;
                    continue;
                }
                shell_input(c);
            }
        }
        long long now = now_ms();
        if (now - next > 5 * VM_TICK_MS)
            next = now;
        while (now >= next) {
            shell_tick();
            next += VM_TICK_MS;
        }
    }
    return 0;
}
