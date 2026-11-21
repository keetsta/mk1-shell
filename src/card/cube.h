/* The screensaver: a wireframe cube turning in perspective, antialiased on the matrix. */
#ifndef MK1_CUBE_H
#define MK1_CUBE_H

#include <stdint.h>

/* the spin rates are whole multiples of 0.01 rad/s, so the motion repeats every 2pi/0.01 s; time wrapped on
   it stays precise in a float however long the card is left alone */
#define CUBE_PERIOD_MS 628319u

/* perceived levels 0..255 at t seconds, row 0 on top, column 0 on the left */
void cube_render(float t, uint8_t lum[9][16]);
/* the same as levels 0..1, ms after the cube came on */
void cube_draw(uint32_t ms, float img[9][16]);

#endif
