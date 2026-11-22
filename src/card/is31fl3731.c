#include "is31fl3731.h"

#include <string.h>

#include "pico/time.h"

#define ADDR 0x74
#define REG_BANK 0xFD
#define BANK_FUNC 0x0B
#define FUNC_CONFIG 0x00
#define FUNC_PICTURE 0x01
#define FUNC_AUTOPLAY1 0x02
#define FUNC_AUTOPLAY2 0x03
#define FUNC_AUDIOSYNC 0x06
#define FUNC_SHUTDOWN 0x0A
#define MODE_AUTOPLAY 0x08 /* configuration: auto frame play from frame 1 */
#define FRAME_ENABLE 0x00 /* 18 bytes, one bit per LED */
#define FRAME_BLINK 0x12  /* 18 bytes */
#define FRAME_PWM 0x24    /* 144 bytes */

static i2c_inst_t *bus;
static uint8_t back = 1;

static int put(const uint8_t *buf, size_t n) {
    return i2c_write_timeout_us(bus, ADDR, buf, n, false, 20000) == (int)n ? 0 : -1;
}

static int reg(uint8_t r, uint8_t v) {
    uint8_t b[2] = {r, v};
    return put(b, 2);
}

static int bank(uint8_t b) { return reg(REG_BANK, b); }

static int fill(uint8_t start, uint8_t value, size_t n) {
    uint8_t b[1 + 144];
    b[0] = start;
    memset(b + 1, value, n);
    return put(b, n + 1);
}

int is31_init(i2c_inst_t *i2c) {
    bus = i2c;
    back = 1;
    if (bank(BANK_FUNC) || reg(FUNC_SHUTDOWN, 0)) return -1;
    sleep_ms(1);
    for (uint8_t f = 0; f < 2; f++)
        if (bank(f) || fill(FRAME_ENABLE, 0xFF, 18) || fill(FRAME_BLINK, 0, 18) || fill(FRAME_PWM, 0, 144))
            return -1;
    if (bank(BANK_FUNC) || reg(FUNC_CONFIG, 0) || reg(FUNC_PICTURE, 0) || reg(FUNC_AUDIOSYNC, 0))
        return -1;
    return reg(FUNC_SHUTDOWN, 1);
}

int is31_autoplay(const uint8_t pwm[][9][16], int n, uint8_t delay) {
    uint8_t b[1 + 144];
    b[0] = FRAME_PWM;
    for (int f = 0; f < n; f++) {
        memcpy(b + 1, pwm[f], 144);
        if (bank((uint8_t)f) || fill(FRAME_ENABLE, 0xFF, 18) || fill(FRAME_BLINK, 0, 18) || put(b, sizeof b))
            return -1;
    }
    /* frame count 0 plays all eight, loop count 0 for ever */
    if (bank(BANK_FUNC) || reg(FUNC_AUTOPLAY1, (uint8_t)(n & 7)) || reg(FUNC_AUTOPLAY2, delay)) return -1;
    return reg(FUNC_CONFIG, MODE_AUTOPLAY);
}

int is31_show(const uint8_t pwm[9][16]) {
    /* the card wires LED C(r+1)-(c+1) at row r, column c (mk1-card design.py), the same as the Adafruit
       16x9 driver's x + y * 16, so the PWM block is plain row-major */
    uint8_t b[1 + 144];
    b[0] = FRAME_PWM;
    memcpy(b + 1, pwm, 144);
    if (bank(back) || put(b, sizeof b) || bank(BANK_FUNC) || reg(FUNC_PICTURE, back)) return -1;
    back ^= 1;
    return 0;
}
