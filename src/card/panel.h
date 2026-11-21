/* What the LEDs get: perceived levels (0..255) through the brightness and a 2.2 gamma into PWM, capped so
 * the whole screen never draws more than the driver can shed. All 144 LEDs at full PWM are ~0.28 A and
 * ~70 C in the IS31FL3731; the cap keeps any picture under 40 % of that. */
#ifndef MK1_PANEL_H
#define MK1_PANEL_H

#include <stdint.h>

#define PANEL_CAP (144u * 255u * 2u / 5u)

void panel_pwm(const uint8_t lum[9][16], float brightness, uint8_t pwm[9][16]);

#endif
