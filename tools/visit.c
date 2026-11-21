/* The card's visit for `make preview`: boot, the prompt and the address running by, a press for the address at
 * once, a terminal opening the port, sudo and its dragons, ^D, the cube after 5 minutes alone, a press to wake
 * it, and sleep after an hour. */
#include "cardsim.h"

#include "../src/card/own.h"

const char *visit_screen(void)
{
    static const char *const FACE[] = {"boot", "own", "link", "shell"};
    static const char *const OWN[] = {"idle", "saver", "sleep"};
    return face_mode() == FACE_OWN ? OWN[own_mode()] : FACE[face_mode()];
}

void visit(void)
{
    sim_run(14000, 1);              /* boot, the prompt, the address running by */
    sim.button = 1;                 /* a press: the address at once */
    sim_run(60, 1);
    sim.button = 0;
    sim_run(7000, 1);
    sim.linked = 1;                 /* a terminal opens the port */
    sim_run(1500, 1);
    sim_type("sudo make me a sandwich\r");
    sim_run(7500, 1);
    sim_type("\x04");               /* ^D: the card's own screen again */
    sim_run(3000, 1);
    sim.linked = 0;                 /* the terminal closes and nobody touches the card: the cube after 5 minutes */
    sim_run(5 * 60 * 1000 - 2000, 0);
    sim_run(6000, 1);
    sim.button = 1;                 /* a press: the prompt again, without the address */
    sim_run(60, 1);
    sim.button = 0;
    sim_run(2000, 1);
    sim_run(61 * 60 * 1000, 0);     /* an hour alone: sleep */
    sim_run(8000, 1);
}
