/* The card's shell: a line editor over the serial port, the manual, tasks, and the MK-1 chip.
 * Single-threaded and non-blocking: the platform feeds every received byte to shell_input and calls
 * shell_tick every 20 ms.
 */
#ifndef MK1_SHELL_H
#define MK1_SHELL_H

/* the firmware's version: the page shows it next to the latest release, whose tag is v<this>.
   Bump it with every release */
#define MK1_VERSION "0.1.0"

#include <stdint.h>

void shell_init(void);
void shell_input(char c);
void shell_tick(void);
/* a program is running in the foreground (the platform may route keys to fake inputs) */
int shell_running(void);
/* a terminal has just opened the port (DTR rose on the card): the banner and the prompt again, since
   whatever the shell printed before went nowhere */
void shell_connect(void);
/* what the card's screen shows now, perceived levels 0..255 (row 0 on top, column 0 on the left); the web
   page gets it when it changes. The shell's own matrix goes only to hal_matrix: a platform whose screen
   is just that matrix hands it back with shell_screen_rows */
void shell_screen(const uint8_t lum[9][16]);
void shell_screen_rows(const uint16_t rows[9]);
/* the terminal went away (DTR dropped on the card): stop sending frames to the web page */
void shell_disconnect(void);

#endif
