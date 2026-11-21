/* The MK-1 card (mk1-card): RP2350A, 12 MHz ABM8-272-T3 crystal, W25Q128JV flash, the core on an external
 * 1.1 V LDO. i2c0 on GPIO4/5 carries the IS31FL3731 (0x74) and the AT24C02 (0x50, WP on GPIO8), the light
 * sensor sits on GPIO26 (ADC0) with its range switch on GPIO27, and the only button is BOOTSEL on QSPI_SS.
 * No LED, UART or SPI defaults: nothing is wired there.
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_MK1_H
#define _BOARDS_MK1_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define PICO_RP2350A 1

#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 4
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 5
#endif

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

// a longer crystal start-up wait costs a few milliseconds at boot and spares a board whose crystal starts slow
#ifndef PICO_XOSC_STARTUP_DELAY_MULTIPLIER
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
