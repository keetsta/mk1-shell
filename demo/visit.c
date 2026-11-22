/* The demo's visit for `make preview-demo`: boot, the prompt, a game of pong, a hold to the cube, a terminal
 * opening the port, sudo and its dragons, ^D, sleep. */
#include "../tools/cardsim.h"

#include <stdio.h>

#include "demo.h"

const char *visit_screen(void)
{
    static const char *const FACE[] = {"boot", "own", "link", "shell", "return"};
    static const char *const OWN[] = {"idle", "saver", "sleep"};
    static char game[16];
    if (face_mode() != FACE_OWN)
        return FACE[face_mode()];
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

void visit(void)
{
    sim_run(14000, 1);              /* boot, the prompt, the address running by */
    sim.button = 1;                 /* a press: pong */
    sim_run(60, 1);
    sim.button = 0;
    play(24000);                    /* then a hold on the score: the bar fills, back to idle */
    sim_run(1500, 1);
    sim.button = 1;                 /* a 2 s hold in idle: the bar fills, the cube */
    sim_run(2200, 1);
    sim.button = 0;
    sim_run(8000, 1);
    sim.button = 1;                 /* a press: idle again */
    sim_run(60, 1);
    sim.button = 0;
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
