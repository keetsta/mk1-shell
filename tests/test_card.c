/* build/test-card: the card's screen and its own screens, the light sensor, the panel and the flash store, on the
 * host. The demo's screens and pong: demo/test_demo.c. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/card/face.h"
#include "../src/card/light.h"
#include "../src/card/own.h"
#include "../src/card/panel.h"
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

/* ---- face, with the card's own screens ---- */

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

static int own(int m) { return face_mode() == FACE_OWN && own_mode() == m; }

/* the prompt's chevron in full: the address is not running */
static int chevron(void)
{
    uint8_t lum[9][16];
    face_render(lum);
    return lum[1][1] == 255 && lum[4][4] == 255 && lum[7][1] == 255;
}

/* ticks of the next ms the address runs */
static int away(uint32_t ms)
{
    int n = 0;
    for (uint32_t end = in.ms + ms; in.ms < end;) {
        run(20);
        n += !chevron();
    }
    return n;
}

static void test_face(void)
{
    memset(&in, 0, sizeof in);
    face_init(0);
    CHECK(face_mode() == FACE_BOOT);
    run(800);
    CHECK(own(OWN_IDLE) && chevron());
    /* a press on the prompt runs the address at once */
    press(60);
    CHECK(own(OWN_IDLE) && !chevron());
    /* and that is all the button does here: holding it starts nothing and changes no screen */
    in.button = 1;
    run(3000);
    in.button = 0;
    run(40);
    CHECK(own(OWN_IDLE));
    /* the address comes back by itself every few seconds */
    CHECK(away(20000) > 300);
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
    /* ^D gives the screen back until someone types; with the terminal there the address runs only when asked */
    face_exit();
    run(20);
    CHECK(own(OWN_IDLE) && away(20000) == 0);
    press(60);
    CHECK(!chevron());
    in.typed = 1;
    run(20);
    CHECK(face_mode() == FACE_SHELL);
    /* a press coming as the card gets the screen back belongs to the screen before */
    in.button = 1;
    face_exit();
    run(20);
    CHECK(own(OWN_IDLE) && chevron());
    in.button = 0;
    run(40);
    CHECK(chevron());
    in.typed = 1;
    run(20);
    CHECK(face_mode() == FACE_SHELL);
    /* the terminal closes: back to the prompt */
    in.linked = 0;
    run(20);
    CHECK(own(OWN_IDLE));
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
    CHECK(own(OWN_IDLE));
    /* left alone: the cube after 5 minutes, sleep after an hour */
    run(5 * 60 * 1000 + 100);
    CHECK(own(OWN_SAVER) && lit() > 10);
    /* a press there only wakes the card: the prompt, not the address */
    press(60);
    CHECK(own(OWN_IDLE) && chevron());
    run(60 * 60 * 1000 + 100);
    CHECK(own(OWN_SLEEP));
    press(60);
    CHECK(own(OWN_IDLE) && chevron());
}

int main(void)
{
    test_store();
    test_light();
    test_panel();
    test_face();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
