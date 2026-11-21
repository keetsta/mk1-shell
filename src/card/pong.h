/* Pong on one button against the card. Holding the button raises your paddle on the left, letting go
 * drops it; the card plays the right. First to 5, then a new match. Holding 2 s between rallies leaves the
 * game: in a rally the button only moves the paddle. */
#ifndef MK1_PONG_H
#define MK1_PONG_H

#include <stdint.h>

#define PONG_WIN 5
#define PONG_QUIT_TICKS 100 /* 2 s of 20 ms ticks */

enum { PONG_SERVE, PONG_PLAY, PONG_POINT, PONG_OVER };

typedef struct {
    float bx, by, vx, vy;   /* the ball, cells and cells per second */
    float you, card;        /* paddle centres, rows */
    float aim;              /* where on its paddle the card means to take the ball this rally */
    float t;                /* seconds in this phase */
    uint16_t held;          /* ticks the button has been down */
    uint32_t rnd;
    uint8_t phase, score[2], quit;
} pong;

void pong_start(pong *p, uint32_t seed);
/* one 20 ms tick */
void pong_tick(pong *p, int button);
/* perceived levels 0..1 */
void pong_draw(const pong *p, float img[9][16]);

#endif
