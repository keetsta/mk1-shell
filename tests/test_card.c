/* build/test-card: the card's own screens, pong, the light sensor, the panel and the flash store, on the host. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/card/face.h"
#include "../src/card/light.h"
#include "../src/card/panel.h"
#include "../src/card/pong.h"
#include "../src/card/store.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- fake NOR flash: erase sets bits, programming can only clear them ---- */
#define SECTORS 4
static uint8_t flash[SECTORS * STORE_SECTOR];
static int erases, programs, tear = -1;   /* tear: that program call stops halfway, as if the power went */

static const uint8_t *f_read(uint32_t off) { return flash + off; }
static int f_erase(uint32_t off)
{
    erases++;
    memset(flash + off, 0xFF, STORE_SECTOR);
    return 0;
}
static int f_program(uint32_t off, const uint8_t *d, uint32_t n)
{
    int torn = programs++ == tear;
    for (uint32_t i = 0; i < (torn ? n / 2 : n); i++)
        flash[off + i] &= d[i];
    return torn ? -1 : 0;
}
static store_t fresh(void)
{
    store_t s = {0, SECTORS, f_read, f_erase, f_program, -1, 0, 0};
    return s;
}

static void test_store(void)
{
    char a[2600], b[2600];
    memset(flash, 0xFF, sizeof flash);
    store_t s = fresh();
    CHECK(!store_load(&s, b, sizeof b));
    for (int i = 0; i < (int)sizeof a; i++)
        a[i] = (char)(i * 7);
    CHECK(store_save(&s, a, sizeof a) == 0);
    s = fresh();
    CHECK(store_load(&s, b, sizeof b) && !memcmp(a, b, sizeof a));
    /* the same bytes again cost no erase */
    int e = erases;
    CHECK(store_save(&s, a, sizeof a) == 0 && erases == e);
    /* round the ring more than once: the newest always wins */
    for (int k = 1; k <= 9; k++) {
        a[0] = (char)k;
        CHECK(store_save(&s, a, sizeof a) == 0);
    }
    s = fresh();
    CHECK(store_load(&s, b, sizeof b) && b[0] == 9);
    /* power lost while the header page was going in, or while the body was: the last good record loads */
    for (int which = 0; which < 2; which++) {
        a[0] = 42;
        tear = programs + which;
        CHECK(store_save(&s, a, sizeof a) == -1);
        tear = -1;
        store_t t = fresh();
        CHECK(store_load(&t, b, sizeof b) && b[0] == 9);
    }
    /* and the next save after a torn one lands */
    s = fresh();
    store_load(&s, b, sizeof b);
    a[0] = 43;
    CHECK(store_save(&s, a, sizeof a) == 0);
    s = fresh();
    CHECK(store_load(&s, b, sizeof b) && b[0] == 43);
    /* a record of another size (a different firmware's state) is not loaded */
    s = fresh();
    CHECK(!store_load(&s, b, 100));
}

/* ---- light ---- */

/* the ADC count a given illuminance gives in a range, for the typical part */
static uint16_t raw_at(float lx, int bright)
{
    float v = lx * 0.2357e-6f * (bright ? 900.9f : 9090.9f), r = v / 3.3f * 4095.0f;
    return (uint16_t)(r > 4095 ? 4095 : r);
}

static uint16_t settle(light_t *l, float lx)
{
    for (int i = 0; i < 400; i++)
        light_update(l, raw_at(lx, l->bright));
    return light_port(l);
}

static void test_light(void)
{
    light_t l = {0};
    CHECK(settle(&l, 0) == 0 && fabsf(light_brightness(&l) - 0.4f) < 0.01f);
    uint16_t room = settle(&l, 300);
    CHECK(room > 600 && room < 670 && !l.bright);
    uint16_t sun = settle(&l, 5000);
    CHECK(l.bright && sun > 900 && sun < 990 && light_brightness(&l) > 0.99f);
    CHECK(settle(&l, 300) == room && !l.bright);
    /* the port only grows with light, across the range switch too */
    uint16_t last = 0;
    int up = 1;
    for (float lx = 0.5f; lx < 12000; lx *= 1.3f) {
        uint16_t p = settle(&l, lx);
        up &= p >= last;
        last = p;
    }
    CHECK(up && last == 1023);
}

/* ---- panel ---- */

static void test_panel(void)
{
    uint8_t lum[9][16], pwm[9][16];
    memset(lum, 0, sizeof lum);
    panel_pwm(lum, 1, pwm);
    CHECK(pwm[4][7] == 0);
    lum[4][7] = 255;
    panel_pwm(lum, 1, pwm);
    CHECK(pwm[4][7] == 255 && pwm[0][0] == 0);
    panel_pwm(lum, 0.4f, pwm);
    CHECK(pwm[4][7] == (uint8_t)(255 * powf(0.4f, 2.2f) + 0.5f));
    memset(lum, 255, sizeof lum);
    panel_pwm(lum, 1, pwm);
    unsigned sum = 0;
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 16; c++)
            sum += pwm[r][c];
    CHECK(sum <= PANEL_CAP + 72 && pwm[8][15] > 90);
}

/* ---- pong ---- */

/* a player with a human reaction: holds the button while the ball, as it was 200 ms ago, is above the paddle */
static float seen[10];
static int bot(const pong *p, int t)
{
    seen[t % 10] = p->by;
    return seen[(t + 1) % 10] < p->you - 0.5f;
}

static void test_pong(void)
{
    pong p;
    pong_start(&p, 7);
    int you = 0, card = 0, matches = 0, inside = 1, won = 0, lost = 0;
    for (int t = 0; t < 50 * 600; t++) {
        /* between rallies a person lets go, so the long holds of play never become a way out */
        pong_tick(&p, p.phase == PONG_PLAY && bot(&p, t));
        won += p.score[0] > you;
        lost += p.score[1] > card;
        matches += p.score[0] + p.score[1] < you + card;
        you = p.score[0];
        card = p.score[1];
        inside &= p.by >= 0 && p.by <= 8 && p.you >= 1 && p.you <= 7 && p.card >= 1 && p.card <= 7;
    }
    /* over ten minutes both sides win points, matches end and start again, and nobody is thrown out */
    printf("pong, ten minutes of a 200 ms player: %d won, %d lost\n", won, lost);
    CHECK(inside && won >= 3 && lost >= 3 && matches >= 2 && !p.quit);

    /* in a rally the button only raises the paddle, however long it is held */
    pong_start(&p, 7);
    while (p.phase != PONG_PLAY)
        pong_tick(&p, 0);
    for (int t = 0; t < 150 && p.phase == PONG_PLAY; t++)
        pong_tick(&p, 1);
    CHECK(!p.quit && p.held == 0);
    /* between rallies 2 s leave, and the score waits for them */
    for (int t = 0; t < 50 * 60 && p.phase != PONG_POINT; t++)
        pong_tick(&p, 0);
    CHECK(p.phase == PONG_POINT);
    for (int t = 0; t < 99; t++)
        pong_tick(&p, 1);
    CHECK(!p.quit && p.phase == PONG_POINT);
    pong_tick(&p, 1);
    CHECK(p.quit);
    /* letting go starts the count again */
    pong_start(&p, 7);
    for (int t = 0; t < 50 * 60 && p.phase != PONG_POINT; t++)
        pong_tick(&p, 0);
    for (int t = 0; t < 300 && p.phase == PONG_POINT; t++)
        pong_tick(&p, t % 90 != 0);
    CHECK(!p.quit);
}

/* ---- face ---- */

static face_in in;

static void run(uint32_t ms)
{
    for (uint32_t end = in.ms + ms; in.ms < end;) {
        in.ms += 20;
        face_tick(&in);
        in.typed = 0;
    }
}

static void press(uint32_t ms)
{
    in.button = 1;
    run(ms);
    in.button = 0;
    run(40);
}

static int lit(void)
{
    uint8_t lum[9][16];
    face_render(lum);
    int n = 0;
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 16; c++)
            n += lum[r][c] > 0;
    return n;
}

static void test_face(void)
{
    memset(&in, 0, sizeof in);
    face_init(0);
    CHECK(face_mode() == FACE_BOOT);
    run(800);
    CHECK(face_mode() == FACE_IDLE && lit() > 0);
    /* a press starts pong; in a rally holding only moves the paddle, between rallies 2 s leave */
    press(60);
    CHECK(face_mode() == FACE_GAME);
    press(2100);
    CHECK(face_mode() == FACE_GAME);
    for (int t = 0; t < 50 * 60 && face_game()->phase != PONG_POINT; t++)
        run(20);
    press(2100);
    CHECK(face_mode() == FACE_IDLE);
    /* a terminal opens the port: the connect animation, then the shell's own pixels, exactly */
    in.linked = 1;
    run(100);
    CHECK(face_mode() == FACE_LINK);
    run(1000);
    CHECK(face_mode() == FACE_SHELL);
    uint16_t rows[9] = {0x8001};
    face_shell(rows);
    run(20);
    uint8_t lum[9][16];
    face_render(lum);
    CHECK(lum[0][0] == 255 && lum[0][15] == 255 && lum[0][1] == 0 && lit() == 2);
    /* the button belongs to the shell now */
    press(60);
    CHECK(face_mode() == FACE_SHELL);
    /* ^D gives the screen back until someone types */
    face_exit();
    run(20);
    CHECK(face_mode() == FACE_IDLE);
    in.typed = 1;
    run(20);
    CHECK(face_mode() == FACE_SHELL);
    /* the terminal closes: back to idle */
    in.linked = 0;
    run(20);
    CHECK(face_mode() == FACE_IDLE);
    /* but a running program keeps the screen until it stops */
    in.linked = 1;
    run(1000);
    in.running = 1;
    run(20);
    in.linked = 0;
    run(2000);
    CHECK(face_mode() == FACE_SHELL);
    in.running = 0;
    run(20);
    CHECK(face_mode() == FACE_IDLE);
    /* left alone: the cube after 5 minutes, sleep after an hour */
    run(5 * 60 * 1000 + 100);
    CHECK(face_mode() == FACE_SAVER && lit() > 10);
    /* a press there only wakes the card, it does not start a game */
    press(60);
    CHECK(face_mode() == FACE_IDLE);
    run(60 * 60 * 1000 + 100);
    CHECK(face_mode() == FACE_SLEEP);
    press(60);
    CHECK(face_mode() == FACE_IDLE);
    /* an abandoned game goes back to idle */
    press(60);
    CHECK(face_mode() == FACE_GAME);
    run(46 * 1000);
    CHECK(face_mode() == FACE_IDLE);
    /* the address runs by every few seconds in idle: the screen is not the prompt for a while */
    int frames = 0, differ = 0;
    uint8_t first[9][16];
    run(20);
    face_render(first);
    for (; frames < 15 * 50; frames++) {
        run(20);
        face_render(lum);
        differ += memcmp(first, lum, sizeof lum) != 0;
    }
    CHECK(differ > 200);
    /* holding 2 s in idle moves on to the cube, the bottom row filling on the way, and letting go after
       that starts no game */
    CHECK(face_mode() == FACE_IDLE);
    in.button = 1;
    run(1500);
    face_render(lum);
    CHECK(face_mode() == FACE_IDLE && lum[8][0] > 0 && lum[8][9] > 0 && lum[8][15] == 0);
    run(600);
    CHECK(face_mode() == FACE_SAVER);
    in.button = 0;
    run(100);
    CHECK(face_mode() == FACE_SAVER);
    /* a press there brings idle back, and that press starts no game either */
    press(60);
    CHECK(face_mode() == FACE_IDLE);
    /* a slow tap is still a tap */
    press(1500);
    CHECK(face_mode() == FACE_GAME);
}

int main(void)
{
    test_store();
    test_light();
    test_panel();
    test_pong();
    test_face();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
