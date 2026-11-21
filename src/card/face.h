/* The card's screen. While a terminal is open the shell owns it; otherwise the card shows its own screens
 * (own.h), each firmware its own set. Here: who has the screen, the boot, two dots meeting when a terminal
 * opens the port, and the afterglow.
 * No hardware here, so the host tests and previews run the same code. */
#ifndef MK1_FACE_H
#define MK1_FACE_H

#include <stdint.h>

enum { FACE_BOOT, FACE_OWN, FACE_LINK, FACE_SHELL };

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
/* FACE_*; on FACE_OWN, own_mode() says which of the card's own screens */
int face_mode(void);
/* perceived levels 0..255, row 0 on top, column 0 on the left */
void face_render(uint8_t lum[9][16]);

#endif
