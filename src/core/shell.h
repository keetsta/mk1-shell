/* The card's shell: a line editor over the serial port, the manual, tasks, and the MK-1 chip.
 * Single-threaded and non-blocking: the platform feeds every received byte to shell_input and calls
 * shell_tick every 20 ms.
 */
#ifndef MK1_SHELL_H
#define MK1_SHELL_H

/* the default for local builds; tagged card builds take their version from the tag */
#ifndef MK1_VERSION
#define MK1_VERSION "0.1.2"
#endif

#include <stdint.h>

void shell_init(void);
void shell_input(char c);
void shell_tick(void);
/* a program is running in the foreground (the platform may route keys to fake inputs) */
int shell_running(void);
/* The source editor is in the foreground. */
int shell_editing(void);
/* Known terminal dimensions. 0 lets the editor query the terminal; negative rows mean plain input. */
void shell_resize(int rows, int cols);
/* a terminal has just opened the port (DTR rose on the card): the banner and the prompt again, since
   whatever the shell printed before went nowhere */
void shell_connect(void);
/* what the card's screen shows now, perceived levels 0..255 (row 0 on top, column 0 on the left); the web
   page gets it when it changes. The shell's own matrix goes only to hal_matrix: a platform whose screen
   is just that matrix hands it back with shell_screen_rows */
void shell_screen(const uint8_t lum[9][16]);
void shell_screen_rows(const uint16_t rows[9]);
/* the terminal went away (DTR dropped on the card): stop sending frames and discard partial sequences */
void shell_disconnect(void);
/* the card's identity for the board (token.h): the chip id and the factory RANDID from OTP, from which the
   card key is made. Only the card has them; without this call `top` says there is no card to sign with */
void shell_card(const uint8_t chipid[8], const uint8_t randid[16]);

#endif
