/* The MK-1 card: the shell over USB, the matrix, the button, the light sensor and the flash. What the shell
 * needs from the card goes through hal.h; what the card shows on its own is face.c.
 *
 * One core, one loop: terminal input goes to the shell as it arrives, the button is sampled every 5 ms, and
 * every 20 ms the shell ticks, the face draws and the matrix gets the frame if it changed. TinyUSB runs from
 * the SDK's background interrupt (pico_stdio_usb); a 1200 baud open makes the loop hand the matrix its update
 * loop and reboot into BOOTSEL. Nothing in the loop waits on a device: a missing matrix is retried, a failed
 * save is dropped.
 */
#include <stddef.h>
#include <string.h>

#include "hardware/adc.h"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/regs/otp_data.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/binary_info.h"
#include "pico/bootrom.h"
#include "pico/flash.h"
#include "pico/rand.h"
#include "pico/stdio/driver.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "pico/usb_reset_config.h"
#include "tusb.h"

#include "../core/hal.h"
#include "../core/shell.h"
#include "face.h"
#include "is31fl3731.h"
#include "light.h"
#include "panel.h"
#include "store.h"
#include "update.h"

#define PIN_SDA 4
#define PIN_SCL 5
#define PIN_EE_WP 8
#define PIN_LUX 26
#define PIN_LUX_RANGE 27
#define ADC_LUX 0

#define TICK_US 20000u
#define BUTTON_US 5000u
#define BUTTON_SAMPLES 4        /* 20 ms the same before the state flips */
#define LINK_SETTLE_US 100000u  /* terminals wiggle DTR while they open */
#define USB_INPUT_CHUNKS 4
#define MATRIX_RETRY_US 500000u
#define WATCHDOG_MS 4000u       /* the longest wait is a sector erase, 400 ms at worst */

bi_decl(bi_2pins_with_func(PIN_SDA, PIN_SCL, GPIO_FUNC_I2C));
bi_decl(bi_1pin_with_name(PIN_EE_WP, "eeprom write protect"));
bi_decl(bi_1pin_with_name(PIN_LUX, "light sensor"));
bi_decl(bi_1pin_with_name(PIN_LUX_RANGE, "light sensor range"));

static light_t light;
static uint8_t button;
static volatile uint8_t reflash;  /* the page touched the port at 1200 baud: a new firmware comes next */

/* ---- flash ------------------------------------------------------------------- */

typedef struct {
    uint32_t off;
    const uint8_t *data;
    uint32_t len;
} flash_op;

static void erase_now(void *p) { flash_range_erase(((const flash_op *)p)->off, STORE_SECTOR); }

static void program_now(void *p) {
    const flash_op *op = p;
    flash_range_program(op->off, op->data, op->len);
}

static int fl_erase(uint32_t off) {
    flash_op op = {off, NULL, 0};
    return flash_safe_execute(erase_now, &op, 100) == PICO_OK ? 0 : -1;
}

static int fl_program(uint32_t off, const uint8_t *data, uint32_t len) {
    flash_op op = {off, data, len};
    return flash_safe_execute(program_now, &op, 100) == PICO_OK ? 0 : -1;
}

static const uint8_t *fl_read(uint32_t off) { return (const uint8_t *)(XIP_BASE + off); }

static_assert(HAL_STATE_MAX <= STORE_MAX, "a shell state the store can't hold would be dropped on every save");
static_assert(MK1_E10_OFFSET + STORE_SECTOR == PICO_FLASH_SIZE_BYTES - MK1_STORE_SECTORS * STORE_SECTOR,
              "the UF2 workaround needs its own sector before the progress ring");

/* the last 32 KB of the 16 MB flash: far past the program, and a new .uf2 never touches it */
static store_t store = {
    .base = PICO_FLASH_SIZE_BYTES - MK1_STORE_SECTORS * STORE_SECTOR,
    .sectors = MK1_STORE_SECTORS,
    .read = fl_read,
    .erase = fl_erase,
    .program = fl_program,
};

/* ---- the shell's card -------------------------------------------------------- */

void hal_write(const char *s, size_t n) {
    /* in pieces: a terminal that reads slowly but steadily never starves the watchdog, and one that stops
       reading costs at most PICO_STDIO_USB_STDOUT_TIMEOUT_US once */
    while (n) {
        int k = n > 256 ? 256 : (int)n;
        stdio_usb.out_chars(s, k);
        watchdog_update();
        s += k;
        n -= (size_t)k;
    }
}

void hal_matrix(const uint16_t rows[9]) { face_shell(rows); }
int hal_button(void) { return button; }
uint16_t hal_lux(void) { return light_port(&light); }
int hal_rows(void) { return 0; }
void hal_exit(void) { face_exit(); }
int hal_load(void *buf, size_t n) { return store_load(&store, buf, (uint32_t)n); }
int hal_save(const void *buf, size_t n) { return store_save(&store, buf, (uint32_t)n); }

/* The card's identity for the board (token.h): the chip id in the order the USB serial number spells it, and
 * RANDID, 128 random bits the factory left in OTP rows 0x004..0x00b. Page 0 of OTP stays readable to any
 * firmware through the window that corrects ECC, 16 bits a row; token.h takes each row low byte first */
static void card_identity(void) {
    pico_unique_board_id_t id;
    uint8_t randid[16];
    const volatile uint16_t *otp = (const volatile uint16_t *)OTP_DATA_BASE;
    pico_get_unique_board_id(&id);
    for (int i = 0; i < 8; i++) {
        uint16_t row = otp[OTP_DATA_RANDID0_ROW + i];
        randid[2 * i] = (uint8_t)row;
        randid[2 * i + 1] = (uint8_t)(row >> 8);
    }
    shell_card(id.id, randid);
}

/* ---- reflashing ------------------------------------------------------------- */

/* In place of the SDK's (PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=0), which reboots into BOOTSEL right here in the
   USB interrupt: the loop does it, once the matrix has the update loop */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding) {
    (void)itf;
    if (coding->bit_rate == PICO_USB_RESET_MAGIC_BAUD_RATE) reflash = 1;
}

/* the driver plays the update loop on its own clock while the ROM bootloader takes the new firmware; without
   a matrix the card just goes */
static void reflash_now(void) {
    static uint8_t lum[UPDATE_FRAMES][9][16], pwm[UPDATE_FRAMES][9][16];
    update_frames(lum);
    for (int f = 0; f < UPDATE_FRAMES; f++) panel_pwm(lum[f], light_brightness(&light), pwm[f]);
    is31_autoplay((const uint8_t(*)[9][16])pwm, UPDATE_FRAMES, UPDATE_DELAY);
    rom_reset_usb_boot_extra(-1, PICO_USB_RESET_BOOTSEL_INTERFACE_DISABLE_MASK, false);
}

/* ---- devices ----------------------------------------------------------------- */

/* DVDD comes from the external 1.1 V LDO and VREG_LX is open: the on-chip regulator only burns ~400 uA, so
   it goes to high impedance (mk1-card notes). Never into the low-power states: with an external DVDD the
   datasheet forbids them */
static void vreg_off(void) {
    hw_set_bits(&powman_hw->vreg_ctrl, POWMAN_PASSWORD_BITS | POWMAN_VREG_CTRL_UNLOCK_BITS);
    while (powman_hw->vreg & POWMAN_VREG_UPDATE_IN_PROGRESS_BITS) tight_loop_contents();
    hw_set_bits(&powman_hw->vreg, POWMAN_PASSWORD_BITS | POWMAN_VREG_HIZ_BITS);
}

static_assert(offsetof(io_qspi_hw_t, io[1].ctrl) == IO_QSPI_GPIO_QSPI_SS_CTRL_OFFSET, "io[1] is QSPI_SS");

/* The button is BOOTSEL on the flash's chip select (through 1 k), as on a Pico: float CS and see if the button
 * pulls it low. Flash is unreachable meanwhile, so this runs from RAM with interrupts off. From
 * pico-examples picoboard/button: Copyright (c) 2020 Raspberry Pi (Trading) Ltd., SPDX-License-Identifier:
 * BSD-3-Clause. */
static bool __no_inline_not_in_flash_func(bootsel_pressed)(void) {
    const uint CS_PIN_INDEX = 1;
    uint32_t flags = save_and_disable_interrupts();
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl, GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (volatile int i = 0; i < 1000; ++i);
    bool pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl, GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    restore_interrupts(flags);
    return pressed;
}

static void light_init(void) {
    adc_init();
    adc_gpio_init(PIN_LUX);
    adc_select_input(ADC_LUX);
    /* low (bright range) or let go, never a logic input: the input buffer stays off either way */
    gpio_init(PIN_LUX_RANGE);
    gpio_disable_pulls(PIN_LUX_RANGE);
    gpio_set_input_enabled(PIN_LUX_RANGE, false);
    gpio_set_drive_strength(PIN_LUX_RANGE, GPIO_DRIVE_STRENGTH_12MA);
    gpio_put(PIN_LUX_RANGE, 0);
}

/* one reading in the current range; the range for the next one is set right after, so it has a whole tick
   to settle (the load and 100 nF take under 1 ms) */
static void light_sample(void) {
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += adc_read();
    light_update(&light, (uint16_t)(sum / 8));
    gpio_set_dir(PIN_LUX_RANGE, light.bright ? GPIO_OUT : GPIO_IN);
}

static void i2c_start(void) {
    /* a reset in the middle of a transfer can leave the driver holding SDA low: clock it free */
    gpio_init(PIN_SDA);
    gpio_init(PIN_SCL);
    for (int i = 0; i < 9 && !gpio_get(PIN_SDA); i++) {
        gpio_set_dir(PIN_SCL, GPIO_OUT);
        sleep_us(5);
        gpio_set_dir(PIN_SCL, GPIO_IN);
        sleep_us(5);
    }
    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
}

/* ---- the loop ---------------------------------------------------------------- */

int main(void) {
    vreg_off();
    /* nothing writes U7 yet: keep the black box read-only */
    gpio_init(PIN_EE_WP);
    gpio_put(PIN_EE_WP, 1);
    gpio_set_dir(PIN_EE_WP, GPIO_OUT);
    stdio_usb_init();
    light_init();
    i2c_start();
    int matrix = is31_init(i2c0) == 0, shown_ok = 0, link_raw = 0, linked = 0, typed = 0, flips = 0;
    static uint8_t lum[9][16], pwm[9][16], shown[9][16];

    uint64_t t0 = time_us_64(), next_tick = t0, next_button = t0, next_retry = t0, link_at = t0;
    face_init(0);
    card_identity();
    shell_init();
    watchdog_enable(WATCHDOG_MS, true);

    for (;;) {
        watchdog_update();
        if (reflash) reflash_now();
        uint64_t now = time_us_64();

        int raw = stdio_usb_connected();
        if (raw != link_raw) {
            link_raw = raw;
            link_at = now;
        }
        if (link_raw != linked && now - link_at >= LINK_SETTLE_US) {
            linked = link_raw;
            if (linked)
                shell_connect();
            else
                shell_disconnect();
        }

        char buf[64];
        for (int chunk = 0; chunk < USB_INPUT_CHUNKS; chunk++) {
            int n = stdio_usb.in_chars(buf, sizeof buf);
            if (n <= 0) break;
            typed = 1;
            for (int i = 0; i < n; i++) shell_input(buf[i]);
        }
        now = time_us_64();

        if (now >= next_button) {
            next_button = now - next_button > 4 * BUTTON_US ? now + BUTTON_US : next_button + BUTTON_US;
            int b = bootsel_pressed();
            flips = b == button ? 0 : flips + 1;
            if (flips >= BUTTON_SAMPLES) {
                button = (uint8_t)b;
                flips = 0;
            }
        }

        if (now >= next_tick) {
            /* after a long stall (a flash erase) carry on from now rather than race through the backlog */
            next_tick = now - next_tick > 5 * TICK_US ? now + TICK_US : next_tick + TICK_US;
            light_sample();
            shell_tick();
            face_in in = {(uint32_t)((now - t0) / 1000), button, (uint8_t)linked, (uint8_t)shell_running(),
                          (uint8_t)typed, get_rand_32()};
            typed = 0;
            face_tick(&in);
            face_render(lum);
            /* the page shows what the LEDs show, the card's own screens included, not just the shell's matrix */
            shell_screen(lum);
            panel_pwm(lum, light_brightness(&light), pwm);
            if (!matrix && now >= next_retry) {
                next_retry = now + MATRIX_RETRY_US;
                matrix = is31_init(i2c0) == 0;
                shown_ok = 0;
            }
            if (matrix && (!shown_ok || memcmp(pwm, shown, sizeof pwm))) {
                matrix = is31_show(pwm) == 0;
                shown_ok = matrix;
                memcpy(shown, pwm, sizeof pwm);
            }
        }
        sleep_us(250);
    }
}
