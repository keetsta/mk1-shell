/* The light sensor: a TEMT6200FX01 phototransistor into a 10 k load, or 10 k || 1 k with GPIO27 pulled low
 * for bright light, read by ADC0. Raw readings become lux, the shell's 0..1023 `lux` port and the matrix
 * brightness. No hardware here: the platform takes the readings and switches the range. */
#ifndef MK1_LIGHT_H
#define MK1_LIGHT_H

#include <stdint.h>

typedef struct {
    uint8_t bright;       /* the range the next reading is taken in: 1 = GPIO27 low */
    uint8_t primed;
    float lg, lg_slow;    /* log10 lux: quick for the port, slow for the brightness */
} light_t;

/* raw: a 12-bit ADC reading taken in l->bright; switches the range for the next reading when needed */
void light_update(light_t *l, uint16_t raw);
float light_lux(const light_t *l);
/* the `lux` port: log scale, 1 lx = 0, 10000 lx = 1023 */
uint16_t light_port(const light_t *l);
/* matrix brightness 0..1; never below the floor, so a dead sensor still leaves a readable screen */
float light_brightness(const light_t *l);

#endif
