/* The card's screen on the host: the shell core and the card's own screens run a scripted visit (boot, the
 * address in idle, a game of pong, a hold to the cube, a terminal opening, sudo and its dragons, ^D, sleep),
 * and every other tick goes to stdout as a raw RGB frame of the front of the card, LEDs as the eye sees
 * their PWM.
 *   build/cardsim | ffmpeg -f rawvideo -pix_fmt rgb24 -s 408x240 -r 25 -i - preview.mp4
 * Mode changes go to stderr with their times.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "../src/card/face.h"
#include "../src/card/panel.h"
#include "../src/card/pong.h"
#include "../src/core/hal.h"
#include "../src/core/shell.h"

#define PX 10                      /* pixels per mm */
#define PITCH (24)                 /* 2.4 mm */
#define MARGIN 12
#define W (16 * PITCH + 2 * MARGIN)
#define H (9 * PITCH + 2 * MARGIN)

static face_in in;
static int frame_no, video_frames, last_mode = -1, last_score = -1;
static unsigned char img[H][W][3];
static const char *const MODES[] = {"boot", "idle", "game", "saver", "sleep", "link", "shell"};

void hal_write(const char *s, size_t n) { (void)s, (void)n; }
void hal_matrix(const uint16_t rows[9]) { face_shell(rows); }
int hal_button(void) { return in.button; }
uint16_t hal_lux(void) { return 600; }
int hal_rows(void) { return 0; }
void hal_exit(void) { face_exit(); }
int hal_load(void *buf, size_t n) { (void)buf, (void)n; return 0; }
void hal_save(const void *buf, size_t n) { (void)buf, (void)n; }

static void put(int x, int y, const unsigned char c[3])
{
    if (x >= 0 && x < W && y >= 0 && y < H)
        memcpy(img[y][x], c, 3);
}

/* white mask, each 0603 LED a 1.6 x 0.8 mm body: pale when dark, flat red-orange when lit */
static void emit(void)
{
    static const unsigned char mask[3] = {244, 244, 241}, body[3] = {228, 226, 220}, led[3] = {255, 74, 18};
    uint8_t lum[9][16], pwm[9][16];
    face_render(lum);
    panel_pwm(lum, 1.0f, pwm);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            put(x, y, mask);
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 16; c++) {
            float v = powf(pwm[r][c] / 255.0f, 1 / 2.2f);
            unsigned char col[3];
            for (int k = 0; k < 3; k++)
                col[k] = (unsigned char)(body[k] + (led[k] - body[k]) * v + 0.5f);
            int cx = MARGIN + c * PITCH + PITCH / 2, cy = MARGIN + r * PITCH + PITCH / 2;
            for (int dy = -4; dy < 4; dy++)
                for (int dx = -8; dx < 8; dx++)
                    put(cx + dx, cy + dy, col);
        }
    fwrite(img, 1, sizeof img, stdout);
}

/* ms of card time; frames only while `show`, so the long waits for the saver and sleep cost nothing */
static void run(uint32_t ms, int show)
{
    for (uint32_t end = in.ms + ms; in.ms < end;) {
        in.ms += 20;
        shell_tick();
        in.running = (uint8_t)shell_running();
        face_tick(&in);
        in.typed = 0;
        const pong *g = face_game();
        int score = face_mode() == FACE_GAME ? g->score[0] * 16 + g->score[1] : -1;
        if (face_mode() != last_mode || score != last_score) {
            last_mode = face_mode();
            last_score = score;
            fprintf(stderr, "video %6.2f s  card %7.2f s  %s", video_frames / 25.0, in.ms / 1000.0, MODES[last_mode]);
            if (score >= 0)
                fprintf(stderr, " %d:%d", g->score[0], g->score[1]);
            fprintf(stderr, "\n");
        }
        if (show && frame_no++ % 2 == 0) {
            emit();
            video_frames++;
        }
    }
}

static void type(const char *s)
{
    for (; *s; s++)
        shell_input(*s);
    in.typed = 1;
}

/* a stand-in player: holds the button while the ball is above the paddle, lets go between rallies; after
   `ms` it plays on to the end of the rally and holds on the score to leave */
static void play(uint32_t ms)
{
    const pong *g = face_game();
    for (uint32_t end = in.ms + ms; in.ms < end || g->phase == PONG_PLAY;) {
        in.button = g->phase == PONG_PLAY && g->by < g->you - 0.6f;
        run(20, 1);
    }
    in.button = 1;
    while (face_mode() == FACE_GAME)
        run(20, 1);
    in.button = 0;
}

int main(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    face_init(0);
    shell_init();
    run(14000, 1);              /* boot, the prompt, the address running by */
    in.button = 1;              /* a press: pong */
    run(60, 1);
    in.button = 0;
    play(24000);                /* then a hold on the score: the bar fills, back to idle */
    run(1500, 1);
    in.button = 1;              /* a 2 s hold in idle: the bar fills, the cube */
    run(2200, 1);
    in.button = 0;
    run(8000, 1);
    in.button = 1;              /* a press: idle again */
    run(60, 1);
    in.button = 0;
    run(1500, 1);
    in.linked = 1;              /* a terminal opens the port */
    run(1500, 1);
    type("sudo make me a sandwich\r");
    run(7500, 1);
    type("\x04");               /* ^D: the card's own screen again */
    run(3000, 1);
    in.linked = 0;              /* the terminal closes, nobody touches the card for an hour: sleep */
    run(61 * 60 * 1000, 0);
    run(8000, 1);
    return 0;
}
