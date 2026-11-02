/* The card's shell: a line editor over the serial port, the manual, tasks, and the MC-1 chip.
 * Single-threaded and non-blocking: the platform feeds every received byte to shell_input and calls
 * shell_tick every 20 ms.
 */
#ifndef MC1_SHELL_H
#define MC1_SHELL_H

/* the firmware's version: the page shows it next to the latest release, whose tag is v<this>.
   Bump it with every release */
#define MC1_VERSION "0.1.0"

void shell_init(void);
void shell_input(char c);
void shell_tick(void);
/* a program is running in the foreground (the platform may route keys to fake inputs) */
int shell_running(void);
/* the terminal went away (DTR dropped on the card): stop sending frames to the web page */
void shell_disconnect(void);

#endif
