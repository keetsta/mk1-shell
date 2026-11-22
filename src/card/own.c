/* The card's own screens: the prompt with the address, the savers when left alone for 5 minutes (saver.h),
 * and sleep after an hour. The button acts only as it goes down: on the prompt it runs the address at once,
 * on a saver and asleep it wakes the card. Games and screens switched by holding the button are the demo's
 * (demo/). */
#include "own.h"

#include "prompt.h"
#include "saver.h"

#define SAVER_MS (5u * 60u * 1000u)
#define SLEEP_MS (60u * 60u * 1000u)

static int mode, linked;
static uint32_t now, since, rnd;

static void go(int m) {
    mode = m;
    since = now;
    if (m == OWN_IDLE) prompt_start(now);
    if (m == OWN_SAVER) saver_begin(now, rnd);
}

void own_enter(uint32_t ms) {
    now = ms;
    go(OWN_IDLE);
}

int own_mode(void) { return mode; }

void own_tick(const own_in *in) {
    now = in->ms;
    linked = in->linked;
    rnd = in->rnd;
    switch (mode) {
    case OWN_IDLE:
        if (in->pressed)
            prompt_address();
        else if (in->still >= SAVER_MS)
            go(OWN_SAVER);
        break;
    case OWN_SAVER:
        if (in->pressed)
            go(OWN_IDLE);
        else if (in->still >= SLEEP_MS)
            go(OWN_SLEEP);
        break;
    case OWN_SLEEP:
        if (in->pressed) go(OWN_IDLE);
        break;
    }
}

/* no afterglow anywhere: the caret fades itself, the address's letters and the savers' motion would smear */
float own_draw(float img[9][16]) {
    switch (mode) {
    case OWN_IDLE:
        prompt_draw(now, linked, img);
        break;
    case OWN_SAVER:
        saver_frame(now, img);
        break;
    case OWN_SLEEP:
        prompt_sleep((float)(now - since) / 1000.0f, img);
        break;
    }
    return 0;
}
