#include "light.h"

#include <math.h>

#define VREF 3.3f
/* the loads with the ADC's own ~100 k input in parallel (mk1-card notes) */
#define R_DARK 9090.9f    /* 10 k || 100 k */
#define R_BRIGHT 900.9f   /* 10 k || 1 k || 100 k */
/* typical part: 3.3 V across 10 k at about 1400 lx (mk1-card design.py) */
#define AMPS_PER_LX 0.2357e-6f
/* the dark range saturates near 1400 lx: switch above ~1100 lx, back below ~900 lx */
#define UP_AT 3000
#define DOWN_AT 250
#define LG_FLOOR -0.5f    /* 0.3 lx, the dark range's least step */
#define PORT_LG_MAX 4.0f  /* 10000 lx */
#define BR_FLOOR 0.4f
#define BR_LG_LO 0.5f     /* ~3 lx: the floor */
#define BR_LG_HI 3.5f     /* ~3000 lx: full */

void light_update(light_t *l, uint16_t raw) {
    float amps = (float)raw * (VREF / 4095.0f) / (l->bright ? R_BRIGHT : R_DARK);
    float lx = amps / AMPS_PER_LX;
    float lg = lx > 0 ? log10f(lx) : LG_FLOOR;
    if (lg < LG_FLOOR) lg = LG_FLOOR;
    if (!l->primed) {
        l->lg = l->lg_slow = lg;
        l->primed = 1;
    } else {
        l->lg += (lg - l->lg) * 0.5f;          /* settles in a few ticks: programs see a hand over the sensor */
        l->lg_slow += (lg - l->lg_slow) * 0.02f; /* about a second: the screen never flickers with shadows */
    }
    if (!l->bright && raw >= UP_AT)
        l->bright = 1;
    else if (l->bright && raw < DOWN_AT)
        l->bright = 0;
}

float light_lux(const light_t *l) { return powf(10.0f, l->lg); }

uint16_t light_port(const light_t *l) {
    float v = l->lg / PORT_LG_MAX * 1023.0f;
    return (uint16_t)(v < 0 ? 0 : v > 1023 ? 1023 : v + 0.5f);
}

float light_brightness(const light_t *l) {
    if (!l->primed) return 1.0f;
    float k = (l->lg_slow - BR_LG_LO) / (BR_LG_HI - BR_LG_LO);
    k = k < 0 ? 0 : k > 1 ? 1 : k;
    return BR_FLOOR + (1 - BR_FLOOR) * k;
}
