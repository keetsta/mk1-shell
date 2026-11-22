/* What the matrix plays while the card takes a new firmware. On the page's 1200 baud touch main.c hands
 * these frames to the IS31FL3731 and reboots into the ROM bootloader; the driver loops them on its own clock
 * (auto frame play) the whole time the new firmware is written, and the new firmware's is31_init stops it.
 * Eight frames and no in-betweens, so nothing jumps: soft slanted bands drift right a pixel a frame.
 * No hardware here: the frames, how long the driver holds each, and a model of it playing for the previews. */
#ifndef MK1_UPDATE_H
#define MK1_UPDATE_H

#include <stdint.h>

#define UPDATE_FRAMES 8
#define UPDATE_DELAY 7   /* each frame held 7 x 11 ms: the bands drift 13 pixels a second */

/* perceived levels 0..255 of every frame, row 0 on top */
void update_frames(uint8_t lum[UPDATE_FRAMES][9][16]);
/* the driver ms into playing them */
void update_at(uint8_t lum[UPDATE_FRAMES][9][16], uint32_t ms, uint8_t out[9][16]);

#endif
