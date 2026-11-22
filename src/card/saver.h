/* What the card shows when left alone (own.c): one of five savers at random, another every few minutes,
 * fading through dark from one to the next. Drawn every 20 ms tick in order, since life keeps state from tick
 * to tick. Perceived levels 0..1 into a dark img, row 0 on top. */
#ifndef MK1_SAVER_H
#define MK1_SAVER_H

#include <stdint.h>

#define SAVER_TURN_MS (3u * 60u * 1000u)  /* each saver's time before the next */
#define SAVER_FADE_MS 1000u               /* out of one and into the next */

enum { SAVER_CUBE, SAVER_PENDULUMS, SAVER_SCOPE, SAVER_LAVA, SAVER_LIFE, SAVER_N };

/* the card is left alone: a saver at random comes on at ms */
void saver_begin(uint32_t ms, uint32_t seed);
void saver_frame(uint32_t ms, float img[9][16]);
/* the one showing */
int saver_kind(void);

/* one saver by itself, ms after it came on; start it first */
void saver_start(int kind, uint32_t seed);
void saver_draw(int kind, uint32_t ms, float img[9][16]);

#endif
