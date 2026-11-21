/* What the card shows on its own while no terminal has the screen, and which of it when. Each firmware links
 * its own: own.c for the card, demo/own.c for the demo. When the card has the screen is face.c's call. */
#ifndef MK1_OWN_H
#define MK1_OWN_H

#include <stdint.h>

/* the screens every firmware has; the demo numbers its own from OWN_MORE */
enum { OWN_IDLE, OWN_SAVER, OWN_SLEEP, OWN_MORE };

typedef struct {
    uint32_t ms;       /* since power-up */
    uint32_t still;    /* ms since the last press, keystroke, or terminal coming or going */
    uint8_t button;    /* debounced */
    uint8_t pressed;   /* went down this tick; a press that came with the screen is not passed on */
    uint8_t released;  /* came up this tick */
    uint8_t linked;    /* a terminal is there and gave the screen back with ^D */
} own_in;

/* the card has the screen again: boot is over, ^D, or the terminal went away */
void own_enter(uint32_t ms);
/* every 20 ms while the card has the screen */
void own_tick(const own_in *in);
/* perceived levels 0..1 into a dark img; returns how much of the last frame lingers on, 0..1 */
float own_draw(float img[9][16]);
int own_mode(void);

#endif
