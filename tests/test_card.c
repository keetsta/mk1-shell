/* build/test-card: the card's screen and its own screens with the savers, the update loop, the light sensor, the
 * panel and the flash store, on the host. The demo's screens and pong: demo/test_demo.c. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/card/face.h"
#include "../src/card/light.h"
#include "../src/card/own.h"
#include "../src/card/panel.h"
#include "../src/card/prompt.h"
#include "../src/card/saver.h"
#include "../src/card/store.h"
#include "../src/card/update.h"

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
    /* the shell's state at its largest (HAL_STATE_MAX, src/core/hal.h) fills a sector; a byte more is refused */
    static char big[STORE_MAX + 1], back[STORE_MAX];
    for (int i = 0; i < (int)sizeof big; i++)
        big[i] = (char)(i * 13);
    s = fresh();
    store_load(&s, b, sizeof b);
    CHECK(store_save(&s, big, STORE_MAX) == 0);
    s = fresh();
    CHECK(store_load(&s, back, STORE_MAX) && !memcmp(big, back, STORE_MAX));
    CHECK(store_save(&s, big, STORE_MAX + 1) == -1);
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

/* the prompt is up and the address is not running: the small chevrons' tips, dim at least whatever the light
   running through them, and nothing where the big chevron or a letter would start */
static int prompt(void)
{
    if (face_mode() != FACE_OWN || own_mode() != OWN_IDLE || prompt_running())
        return 0;
    uint8_t lum[9][16];
    face_render(lum);
    return lum[4][3] && lum[4][7] && lum[4][11] && !lum[1][1];
}

/* ticks of the next ms the address runs */
static int away(uint32_t ms)
{
    int n = 0;
    for (uint32_t end = in.ms + ms; in.ms < end;) {
        run(20);
        n += prompt_running();
    }
    return n;
}

static void test_transitions(void)
{
    uint8_t previous[9][16], lum[9][16], idle_start[9][16];
    uint16_t rows[9] = {0x8001};
    memset(&in, 0, sizeof in);
    face_init(0);
    run(600);
    CHECK(face_mode() == FACE_BOOT);
    face_render(previous);
    run(20);
    face_render(lum);
    CHECK(prompt() && !memcmp(lum, previous, sizeof lum));
    memcpy(idle_start, lum, sizeof idle_start);

    run(840);
    face_render(previous);
    CHECK(prompt() && !previous[4][14]);
    in.linked = 1;
    run(20);
    face_render(lum);
    CHECK(face_mode() == FACE_LINK && !memcmp(lum, previous, sizeof lum));
    face_shell(rows);
    run(280);
    CHECK(face_mode() == FACE_LINK && lit() == 0);
    run(20);
    face_render(lum);
    CHECK(face_mode() == FACE_SHELL && lum[0][0] == 255 && lum[0][15] == 255 && lit() == 2);

    memcpy(previous, lum, sizeof previous);
    face_exit();
    in.typed = 1;
    run(20);
    face_render(lum);
    CHECK(face_mode() == FACE_RETURN && !memcmp(lum, previous, sizeof lum));
    run(80);
    face_render(lum);
    CHECK(face_mode() == FACE_RETURN && !lum[0][15] && lit() > 2);
    face_exit();
    in.typed = 1;
    run(20);
    in.linked = 0;
    run(140);
    CHECK(face_mode() == FACE_RETURN);
    in.button = 1;
    run(20);
    face_render(lum);
    CHECK(prompt() && !memcmp(lum, idle_start, sizeof lum));

    memset(&in, 0, sizeof in);
    face_init(0);
    in.linked = 1;
    run(600);
    face_render(previous);
    run(20);
    face_render(lum);
    CHECK(face_mode() == FACE_LINK && !memcmp(lum, previous, sizeof lum));
    run(300);
    CHECK(face_mode() == FACE_SHELL);
    in.linked = 0;
    run(20);
    CHECK(face_mode() == FACE_RETURN);
    run(80);
    face_render(previous);
    in.linked = 1;
    run(20);
    face_render(lum);
    CHECK(face_mode() == FACE_LINK && !memcmp(lum, previous, sizeof lum));
    in.running = 1;
    in.linked = 0;
    run(20);
    CHECK(face_mode() == FACE_LINK);
    in.running = 0;
    run(20);
    CHECK(face_mode() == FACE_RETURN);
    run(260);
    CHECK(prompt());
}

static void test_face(void)
{
    memset(&in, 0, sizeof in);
    face_init(0);
    CHECK(face_mode() == FACE_BOOT);
    run(800);
    CHECK(prompt());
    /* a press on the prompt runs the address at once */
    press(60);
    CHECK(own(OWN_IDLE) && prompt_running());
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
    run(280);
    CHECK(prompt() && away(20000) == 0);
    press(60);
    CHECK(own(OWN_IDLE) && prompt_running());
    in.typed = 1;
    run(20);
    CHECK(face_mode() == FACE_SHELL);
    /* a press coming as the card gets the screen back belongs to the screen before */
    in.button = 1;
    face_exit();
    run(280);
    CHECK(prompt());
    in.button = 0;
    run(40);
    CHECK(prompt());
    in.typed = 1;
    run(20);
    CHECK(face_mode() == FACE_SHELL);
    /* the terminal closes: back to the prompt */
    in.linked = 0;
    run(280);
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
    run(280);
    CHECK(own(OWN_IDLE));
    /* left alone: the savers after 5 minutes, sleep after an hour */
    run(5 * 60 * 1000 + 100);
    CHECK(own(OWN_SAVER) && lit() > 10);
    /* a press there only wakes the card: the prompt, not the address */
    press(60);
    CHECK(prompt());
    run(60 * 60 * 1000 + 100);
    CHECK(own(OWN_SLEEP) && lit() > 0);
    press(60);
    CHECK(prompt());
}

/* ---- the loop the matrix plays while the card is reflashed ---- */

static void test_update(void)
{
    static uint8_t lum[UPDATE_FRAMES][9][16];
    uint8_t pwm[9][16], seen[9][16];
    int drift = 1;
    update_frames(lum);
    for (int f = 0; f < UPDATE_FRAMES; f++) {
        /* every frame lights something, within the current the driver can shed */
        unsigned sum = 0;
        panel_pwm(lum[f], 1, pwm);
        for (int r = 0; r < 9; r++)
            for (int c = 0; c < 16; c++)
                sum += pwm[r][c];
        CHECK(sum > 0 && sum <= PANEL_CAP + 72);
        /* the driver holds each frame for its delay */
        update_at(lum, (uint32_t)(f * UPDATE_DELAY * 11 + 5), seen);
        CHECK(!memcmp(seen, lum[f], sizeof seen));
        /* the next frame is this one a pixel to the right, and the last leads back into the first */
        for (int r = 0; r < 9; r++)
            for (int c = 0; c < 15; c++) {
                int d = lum[(f + 1) % UPDATE_FRAMES][r][c + 1] - lum[f][r][c];
                drift &= d >= -1 && d <= 1;
            }
    }
    CHECK(drift && UPDATE_DELAY >= 1 && UPDATE_DELAY <= 63);
}

/* ---- the savers ---- */

static float brightest(float img[9][16], int *lit_px)
{
    float top = 0;
    *lit_px = 0;
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 16; c++) {
            if (img[r][c] > top)
                top = img[r][c];
            *lit_px += img[r][c] > 0.05f;
        }
    return top;
}

static void test_saver(void)
{
    float img[9][16];
    int n;
    /* every saver shows something soon, within the levels the face takes */
    for (int k = 0; k < SAVER_N; k++) {
        saver_start(k, 7);
        for (uint32_t ms = 0; ms <= 3000; ms += 20) {
            memset(img, 0, sizeof img);
            saver_draw(k, ms, img);
        }
        float top = brightest(img, &n);
        CHECK(n >= 6 && top <= 1);
    }
    /* the first saver is the platform's dice, not the clock: a card switched on and left alone would otherwise
       start the same one every time */
    int firsts = 0, distinct = 0;
    for (uint32_t seed = 1; seed <= 8; seed++) {
        saver_begin(300000, seed * 2654435761u);
        firsts |= 1 << saver_kind();
    }
    for (int k = 0; k < SAVER_N; k++)
        distinct += firsts >> k & 1;
    CHECK(distinct >= 3);
    /* left alone: a saver at random, another every turn, never the same twice in a row, fading through dark */
    int kinds = 0, last = -1, repeats = 0, turns = 0, dark_at_turn = 1, dim_before = 1;
    saver_begin(5000, 99);
    for (uint32_t ms = 5000; ms < 5000 + 12 * SAVER_TURN_MS; ms += 200) {
        uint32_t into = (ms - 5000) % SAVER_TURN_MS;
        memset(img, 0, sizeof img);
        saver_frame(ms, img);
        float top = brightest(img, &n);
        if (into == 0) {
            dark_at_turn &= top == 0;
            repeats += saver_kind() == last;
            turns++;
        }
        if (into == SAVER_TURN_MS - 200)
            dim_before &= top <= 0.21f;
        last = saver_kind();
        kinds |= 1 << last;
    }
    CHECK(turns == 12 && !repeats && dark_at_turn && dim_before && kinds == (1 << SAVER_N) - 1);
}

int main(void)
{
    test_store();
    test_light();
    test_panel();
    test_transitions();
    test_face();
    test_update();
    test_saver();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
