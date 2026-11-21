/* IS31FL3731: the 16x9 matrix on i2c0, address 0x74. Two of its eight frames are used: one is shown while
 * the other is written, so a half-written frame never reaches the LEDs. */
#ifndef MK1_IS31FL3731_H
#define MK1_IS31FL3731_H

#include <stdint.h>

#include "hardware/i2c.h"

/* 0 on success: the chip is on, every LED enabled and dark */
int is31_init(i2c_inst_t *i2c);
/* 0 on success; PWM row-major, row 0 on top, column 0 on the left */
int is31_show(const uint8_t pwm[9][16]);

#endif
