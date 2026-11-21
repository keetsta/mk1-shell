cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD mk1 CACHE STRING "")
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/board)
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(mk1 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(CORE ${CMAKE_CURRENT_LIST_DIR}/../core)
file(STRINGS ${CORE}/shell.h MK1_VERSION_LINE REGEX "#define MK1_VERSION ")
string(REGEX MATCH "\"([^\"]+)\"" _ ${MK1_VERSION_LINE})
set(MK1_VERSION ${CMAKE_MATCH_1})

add_executable(mk1
    main.c face.c pong.c cube.c light.c panel.c store.c is31fl3731.c
    ${CORE}/vm.c ${CORE}/sha256.c ${CORE}/tasks.c ${CORE}/font.c ${CORE}/shell.c ${CORE}/content_gen.c)
target_compile_options(mk1 PRIVATE -Wall -Wextra)
target_compile_definitions(mk1 PRIVATE
    USBD_MANUFACTURER="keetsta"
    USBD_PRODUCT="mk-1"
    # a whole screen of the manual fits in the USB buffer instead of trickling out 64 bytes at a time
    CFG_TUD_CDC_TX_BUFSIZE=2048
    CFG_TUD_CDC_RX_BUFSIZE=256
    # one core: flash writes only need this core's interrupts off
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    # all of SCRATCH_Y instead of half: the USB interrupt nests on this stack too
    PICO_STACK_SIZE=0x1000
)
target_link_libraries(mk1 pico_stdlib pico_flash hardware_adc hardware_flash hardware_i2c hardware_watchdog)
pico_enable_stdio_usb(mk1 1)
pico_enable_stdio_uart(mk1 0)
pico_set_program_name(mk1 "mk1")
pico_set_program_version(mk1 "${MK1_VERSION}")
pico_set_program_description(mk1 "keetsta mk-1: a shell over usb-c")
pico_set_program_url(mk1 "https://keetsta.me/mk1")
pico_add_extra_outputs(mk1)
