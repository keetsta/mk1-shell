/* build/test-demo: the demo firmware's own screens and pong, on the host. */
#include <stdio.h>
#include <string.h>

#include "../src/card/face.h"
#include "demo.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

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

static int own(int m) { return face_mode() == FACE_OWN && own_mode() == m; }

static void reset_face(void)
{
    memset(&in, 0, sizeof in);
    face_init(0);
    run(800);
}

static void test_gallery(void)
{
    reset_face();
    CHECK(own(OWN_IDLE) && lit() > 0);
    uint8_t frames[SAVER_N][9][16], lum[9][16];
    for (int i = 0; i < SAVER_N; i++) {
        in.button = 1;
        run(60);
        CHECK(i == 0 ? own(OWN_IDLE) : own(OWN_SAVER) && demo_saver_kind() == i - 1);
        in.button = 0;
        run(300);
        CHECK(own(OWN_SAVER) && demo_saver_kind() == i && lit() > 0);
        face_render(frames[i]);
        for (int j = 0; j < i; j++)
            CHECK(memcmp(frames[i], frames[j], sizeof lum) != 0);
        run(320);
        face_render(lum);
        CHECK(memcmp(frames[i], lum, sizeof lum) != 0);
    }
    press(60);
    CHECK(own(OWN_IDLE));
    press(1500);
    CHECK(own(OWN_SAVER) && demo_saver_kind() == SAVER_CUBE);
    run(30 * 1000);
    CHECK(own(OWN_SAVER) && demo_saver_kind() == SAVER_CUBE);

    reset_face();
    run(5 * 60 * 1000 + 100);
    CHECK(own(OWN_SAVER) && demo_saver_kind() == SAVER_CUBE);
    for (int i = 1; i <= SAVER_N; i++) {
        run(15 * 1000);
        CHECK(own(OWN_SAVER) && demo_saver_kind() == i % SAVER_N);
    }
    press(60);
    run(15 * 1000);
    CHECK(own(OWN_SAVER) && demo_saver_kind() == SAVER_PENDULUMS);
    run(60 * 60 * 1000 + 100);
    CHECK(own(OWN_SLEEP));
    press(2500);
    CHECK(own(OWN_IDLE));
    press(60);
    CHECK(own(OWN_SAVER) && demo_saver_kind() == SAVER_CUBE);
}

static void test_pong_button(void)
{
    reset_face();
    uint8_t lum[9][16];
    in.button = 1;
    run(1500);
    face_render(lum);
    CHECK(own(OWN_IDLE) && lum[8][0] > 0 && lum[8][9] > 0 && lum[8][15] == 0);
    run(600);
    face_render(lum);
    CHECK(own(OWN_IDLE) && lum[8][15] > 0);
    in.button = 0;
    run(20);
    CHECK(own(DEMO_GAME) && demo_game()->phase == PONG_SERVE && demo_game()->you == 4.0f);
    press(2100);
    CHECK(own(DEMO_GAME));
    for (int t = 0; t < 50 * 60 && demo_game()->phase != PONG_POINT; t++)
        run(20);
    CHECK(demo_game()->phase == PONG_POINT);
    press(2100);
    CHECK(own(OWN_IDLE));
    press(60);
    CHECK(own(OWN_SAVER) && demo_saver_kind() == SAVER_CUBE);
    press(2100);
    CHECK(own(DEMO_GAME));
    run(46 * 1000);
    CHECK(own(OWN_IDLE));
}

static void test_face(void)
{
    memset(&in, 0, sizeof in);
    face_init(0);
    CHECK(face_mode() == FACE_BOOT);
    press(2500);
    CHECK(own(OWN_IDLE) && lit() > 0);
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
    run(280);
    CHECK(own(OWN_IDLE));
    in.typed = 1;
    run(20);
    CHECK(face_mode() == FACE_SHELL);
    /* the terminal closes: back to idle */
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
}

int main(void)
{
    test_pong();
    test_gallery();
    test_pong_button();
    test_face();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
