#include "../tools/cardsim.h"

#include <stdio.h>

#include "demo.h"

const char *visit_screen(void)
{
    static const char *const FACE[] = {"boot", "own", "link", "shell", "return"};
    static const char *const OWN[] = {"idle", "saver", "sleep"};
    static const char *const SAVERS[SAVER_N] = {"cube", "pendulums", "scope", "lava", "life"};
    static char game[16];
    if (face_mode() != FACE_OWN)
        return FACE[face_mode()];
    if (own_mode() == OWN_SAVER)
        return SAVERS[demo_saver_kind()];
    if (own_mode() != DEMO_GAME)
        return OWN[own_mode()];
    snprintf(game, sizeof game, "game %d:%d", demo_game()->score[0], demo_game()->score[1]);
    return game;
}

/* a stand-in player: holds the button while the ball is above the paddle, lets go between rallies; after
   `ms` it plays on to the end of the rally and holds on the score to leave */
static void play(uint32_t ms)
{
    const pong *g = demo_game();
    for (uint32_t end = sim.ms + ms; sim.ms < end || g->phase == PONG_PLAY;) {
        sim.button = g->phase == PONG_PLAY && g->by < g->you - 0.6f;
        sim_run(20, 1);
    }
    sim.button = 1;
    while (face_mode() == FACE_OWN && own_mode() == DEMO_GAME)
        sim_run(20, 1);
    sim.button = 0;
}

static void press(uint32_t ms)
{
    sim.button = 1;
    sim_run(ms, 1);
    sim.button = 0;
    sim_run(40, 1);
}

void visit(void)
{
    sim_run(14000, 1);              /* boot, the prompt, the address running by */
    for (int i = 0; i < SAVER_N; i++) {
        press(60);
        sim_run(6000, 1);
    }
    press(60);
    sim_run(1500, 1);
    press(2200);
    play(24000);
    sim_run(1500, 1);
    sim.linked = 1;                 /* a terminal opens the port */
    sim_run(1500, 1);
    sim_type("sudo make me a sandwich\r");
    sim_run(7500, 1);
    sim_type("\x04");               /* ^D: the card's own screen again */
    sim_run(3000, 1);
    sim.linked = 0;                 /* the terminal closes, nobody touches the card for an hour: sleep */
    sim_run(61 * 60 * 1000, 0);
    sim_run(8000, 1);
}
