#ifndef MK1_DEMO_H
#define MK1_DEMO_H

#include "../src/card/own.h"
#include "../src/card/saver.h"
#include "pong.h"

enum { DEMO_GAME = OWN_MORE };

int demo_saver_kind(void);
/* the game in progress, for the preview's stand-in player */
const pong *demo_game(void);

#endif
