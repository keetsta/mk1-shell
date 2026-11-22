/* The card's screen on the host: the shell core and the card's own screens run a scripted visit (cardsim.h),
 * and every other tick goes to stdout as a raw RGB frame of the front of the card, LEDs as the eye sees
 * their PWM.
 *   build/cardsim | ffmpeg -f rawvideo -pix_fmt rgb24 -s 408x240 -r 25 -i - preview.mp4
 * What the screen shows goes to stderr with the times, whenever it changes.
 *   build/cardsim update   instead plays what the matrix loops while the card takes a new firmware (update.h)
 *   build/cardsim savers   instead plays each saver the card picks from when left alone (saver.h)
 */
#include "cardsim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "../src/card/panel.h"
#include "../src/card/saver.h"
#include "../src/card/update.h"
#include "../src/core/hal.h"
#include "../src/core/shell.h"

#define PX 10                      /* pixels per mm */
#define PITCH (24)                 /* 2.4 mm */
#define MARGIN 12
#define W (16 * PITCH + 2 * MARGIN)
#define H (9 * PITCH + 2 * MARGIN)

face_in sim;
static int frame_no, video_frames;
static char shows[32];
static unsigned char img[H][W][3];

void hal_write(const char *s, size_t n) { (void)s, (void)n; }
void hal_matrix(const uint16_t rows[9]) { face_shell(rows); }
int hal_button(void) { return sim.button; }
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
static void emit(uint8_t lum[9][16])
{
    static const unsigned char mask[3] = {244, 244, 241}, body[3] = {228, 226, 220}, led[3] = {255, 74, 18};
    uint8_t pwm[9][16];
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

void sim_run(uint32_t ms, int show)
{
    for (uint32_t end = sim.ms + ms; sim.ms < end;) {
        sim.ms += 20;
        shell_tick();
        sim.running = (uint8_t)shell_running();
        face_tick(&sim);
        sim.typed = 0;
        const char *now = visit_screen();
        if (strcmp(now, shows)) {
            snprintf(shows, sizeof shows, "%s", now);
            fprintf(stderr, "video %6.2f s  card %7.2f s  %s\n", video_frames / 25.0, sim.ms / 1000.0, shows);
        }
        if (show && frame_no++ % 2 == 0) {
            uint8_t lum[9][16];
            face_render(lum);
            emit(lum);
            video_frames++;
        }
    }
}

void sim_type(const char *s)
{
    for (; *s; s++)
        shell_input(*s);
    sim.typed = 1;
}

/* the loop the matrix plays while the card is reflashed, for 6 s */
static void updates(void)
{
    static uint8_t frames[UPDATE_FRAMES][9][16];
    uint8_t lum[9][16];
    update_frames(frames);
    for (uint32_t ms = 0; ms < 6000; ms += 40) {
        update_at(frames, ms, lum);
        emit(lum);
    }
}

/* every saver the card picks from when left alone, 16 s each from its start, a dark second
   between; the card draws them every 20 ms tick and the video takes every other */
static void savers(void)
{
    static const char *const NAMES[SAVER_N] = {"cube", "pendulums", "scope", "lava", "life"};
    float img[9][16];
    uint8_t lum[9][16];
    for (int k = 0; k < SAVER_N; k++) {
        fprintf(stderr, "video %6.2f s  %s\n", video_frames / 25.0, NAMES[k]);
        saver_start(k, 12345);
        for (uint32_t ms = 0; ms < 16000; ms += 20) {
            memset(img, 0, sizeof img);
            saver_draw(k, ms, img);
            if (ms % 40)
                continue;
            for (int y = 0; y < 9; y++)
                for (int x = 0; x < 16; x++)
                    lum[y][x] = (uint8_t)(img[y][x] >= 1 ? 255 : img[y][x] * 255.0f + 0.5f);
            emit(lum);
            video_frames++;
        }
        memset(lum, 0, sizeof lum);
        for (int i = 0; i < 25; i++, video_frames++)
            emit(lum);
    }
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc > 1 && !strcmp(argv[1], "update")) {
        updates();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "savers")) {
        savers();
        return 0;
    }
    face_init(0);
    shell_init();
    visit();
    return 0;
}
