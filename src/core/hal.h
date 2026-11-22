/* What the shell needs from the card. src/host implements it for a Mac terminal; the RP2350 build
 * implements it over TinyUSB CDC, the IS31FL3731 and the GPIOs (see the notes in mk1-card/CLAUDE.md).
 */
#ifndef MK1_HAL_H
#define MK1_HAL_H

#include <stddef.h>
#include <stdint.h>

/* serial out; the shell already writes \r\n */
void hal_write(const char *s, size_t n);
/* 9 rows, bit 15 = leftmost column. On the card the driver caps full-screen brightness itself:
   all 144 LEDs at full current is 0.28 A and ~70 C in the IS31FL3731 */
void hal_matrix(const uint16_t rows[9]);
int hal_button(void);
/* light level 0..1023 */
uint16_t hal_lux(void);
/* rows of text the terminal shows at once; longer output (the manual) is paged to fit.
   0 = no paging: the card's terminals scroll back on their own */
int hal_rows(void);
/* ^D at an empty prompt. The emulator quits; the card goes back to its idle screen, and the shell
   carries on as soon as someone types again */
void hal_exit(void);
/* persistent state (unlocked chapters, the program); load returns 0 unless a valid state of exactly n bytes
   is there. save returns 0 on success or -1 on failure. The state must fit in a 4 KB flash sector. */
#define HAL_STATE_MAX 4080
int hal_load(void *buf, size_t n);
int hal_save(const void *buf, size_t n);

#endif
