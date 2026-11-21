/* The prompt the card shows on its own: a chevron and a blinking cursor, and every few seconds the address to
 * go to running by. Asleep, only the cursor, breathing. Every firmware's own screens draw it. */
#ifndef MK1_PROMPT_H
#define MK1_PROMPT_H

#include <stdint.h>

/* the prompt comes on: the cursor blinks from now, the address first runs a little later */
void prompt_start(uint32_t ms);
/* the address on the next draw, unless it is running already */
void prompt_address(void);
/* one tick of the prompt; 1 while the address runs, its letters want no afterglow. With a terminal there the
   address only runs when asked */
int prompt_draw(uint32_t ms, int linked, float img[9][16]);
/* t seconds asleep */
void prompt_sleep(float t, float img[9][16]);

#endif
