/* The prompt the card shows on its own: three small chevrons a light runs through now and then, a caret after
 * them that fades out and in rather than snapping, and every few seconds the address to go to running by.
 * Asleep, only the caret, breathing. Every firmware's own screens draw it. */
#ifndef MK1_PROMPT_H
#define MK1_PROMPT_H

#include <stdint.h>

/* the prompt comes on: the caret blinks from now, the address first runs a little later */
void prompt_start(uint32_t ms);
/* the address on the next draw, unless it is running already */
void prompt_address(void);
/* the waiting frame at age ms, independent of the address and prompt state */
void prompt_idle(uint32_t age, float img[9][16]);
/* one tick of the prompt; 1 while the address runs. With a terminal there the address only runs when asked */
int prompt_draw(uint32_t ms, int linked, float img[9][16]);
/* the address is running by */
int prompt_running(void);
/* t seconds asleep */
void prompt_sleep(float t, float img[9][16]);

#endif
