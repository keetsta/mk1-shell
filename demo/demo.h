/* The demo firmware's own screens (demo/own.c, in place of src/card/own.c): the card's prompt, cube and sleep,
 * and on the one button pong on a tap and the cube on a 2 s hold. */
#ifndef MK1_DEMO_H
#define MK1_DEMO_H

#include "../src/card/own.h"
#include "pong.h"

enum { DEMO_GAME = OWN_MORE };

/* the game in progress, for the preview's stand-in player */
const pong *demo_game(void);

#endif
