/* The card's screen. While a terminal is open the shell owns it; otherwise the card shows its own modes:
 * a prompt with the address to go to, pong on a tap, the cube on a 2 s hold or when left alone, and at last
 * sleep.
 * No hardware here, so the host tests and previews run the same code. */
#ifndef MK1_FACE_H
#define MK1_FACE_H

#include <stdint.h>

#include "pong.h"

enum { FACE_BOOT, FACE_IDLE, FACE_GAME, FACE_SAVER, FACE_SLEEP, FACE_LINK, FACE_SHELL };

typedef struct {
    uint32_t ms;       /* since power-up */
    uint8_t button;    /* debounced */
    uint8_t linked;    /* a terminal holds the port (DTR) */
    uint8_t running;   /* the shell runs a program */
    uint8_t typed;     /* the terminal sent something since the last tick */
} face_in;

void face_init(uint32_t ms);
/* every 20 ms */
void face_tick(const face_in *in);
/* the shell's matrix, on screen while the shell owns it */
void face_shell(const uint16_t rows[9]);
/* ^D at the prompt: the card's own screen until someone types again */
void face_exit(void);
int face_mode(void);
/* perceived levels 0..255, row 0 on top, column 0 on the left */
void face_render(uint8_t lum[9][16]);
/* the game in progress, for the host preview's stand-in player */
const pong *face_game(void);

#endif
