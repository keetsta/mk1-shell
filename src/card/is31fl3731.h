/* IS31FL3731: the 16x9 matrix on i2c0, address 0x74. Two of its eight frames are used: one is shown while
 * the other is written, so a half-written frame never reaches the LEDs. All eight go to the loop the driver
 * plays by itself while the card is reflashed (update.h). */
#ifndef MK1_IS31FL3731_H
#define MK1_IS31FL3731_H

#include <stdint.h>

#include "hardware/i2c.h"

/* 0 on success: the chip is on, every LED enabled and dark */
int is31_init(i2c_inst_t *i2c);
/* 0 on success; PWM row-major, row 0 on top, column 0 on the left */
int is31_show(const uint8_t pwm[9][16]);
/* n frames (1..8) into the driver's frames 1..n, looped on its own clock until is31_init, each held
   delay x 11 ms (1..63). 0 on success */
int is31_autoplay(const uint8_t pwm[][9][16], int n, uint8_t delay);

#endif
