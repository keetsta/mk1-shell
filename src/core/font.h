/* Lowercase pixel font for the 16x9 matrix and a scrolling line built from it. */
#ifndef MC1_FONT_H
#define MC1_FONT_H

#include <stdint.h>

#define MARQUEE_MAX 512

typedef struct {
    uint16_t col[MARQUEE_MAX]; /* bit r = row r lit */
    int ncol, pos;
} marquee;

/* lays the text out with 16 empty columns on both sides, so it enters and leaves the screen */
void marquee_start(marquee *m, const char *text);
/* draws the current position into rows[9] and moves one column on; 0 once the text has left */
int marquee_step(marquee *m, uint16_t rows[9]);
/* 0 if every glyph has 9 rows of equal width, else the first bad character */
char font_check(void);

#endif
