/* The card's screen on the host (tools/cardsim.c) and what a preview brings to it: a visit, the card's in
 * tools/visit.c and the demo's in demo/visit.c. */
#ifndef MK1_CARDSIM_H
#define MK1_CARDSIM_H

#include <stdint.h>

#include "../src/card/face.h"

/* the button and the port as the visit sets them */
extern face_in sim;
/* ms of card time; frames only while `show`, so the long waits for the saver and sleep cost nothing */
void sim_run(uint32_t ms, int show);
/* keys from a terminal */
void sim_type(const char *s);

void visit(void);
/* what the screen shows, for the log on stderr */
const char *visit_screen(void);

#endif
