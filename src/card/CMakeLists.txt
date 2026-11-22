cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD mk1 CACHE STRING "")
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/board)
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(mk1 C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

set(CORE ${CMAKE_CURRENT_LIST_DIR}/../core)
set(DEMO ${CMAKE_CURRENT_LIST_DIR}/../../demo)

if (NOT PICO_FLASH_SIZE_BYTES)
    set(PICO_FLASH_SIZE_BYTES ${PICO_DEFAULT_FLASH_SIZE_BYTES})
endif()
set(MK1_STORE_SECTORS 8)
math(EXPR MK1_E10_OFFSET "${PICO_FLASH_SIZE_BYTES} - (${MK1_STORE_SECTORS} + 1) * 4096")
math(EXPR MK1_E10_SECTOR "0x10000000 + ${MK1_E10_OFFSET}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR MK1_E10_PAGE "${MK1_E10_SECTOR} + 4096 - 256" OUTPUT_FORMAT HEXADECIMAL)
set(MK1_FLASH_BOUNDS "${CMAKE_CURRENT_BINARY_DIR}/mk1_flash_bounds.ld")
file(GENERATE OUTPUT "${MK1_FLASH_BOUNDS}"
     CONTENT "ASSERT(__flash_binary_end <= ${MK1_E10_SECTOR}, \"firmware overlaps the reserved E10 sector\")\n")

set(MK1_RELEASE_VERSION "" CACHE STRING "Version of a tagged mk-1 release")
set(MK1_VERSION "${MK1_RELEASE_VERSION}")
if (MK1_VERSION STREQUAL "")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CORE}/shell.h")
    file(STRINGS ${CORE}/shell.h MK1_VERSION_LINE REGEX "^#define MK1_VERSION ")
    string(REGEX MATCH "\"([^\"]+)\"" _ ${MK1_VERSION_LINE})
    set(MK1_VERSION "${CMAKE_MATCH_1}")
endif()
if (NOT MK1_VERSION MATCHES "^(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$")
    message(FATAL_ERROR "invalid mk-1 version: ${MK1_VERSION}")
endif()

# the card's firmware and the demo's differ only in the card's own screens (own.h), passed after the name
function(mk1_firmware name product description)
    add_executable(${name}
        main.c face.c prompt.c cube.c saver.c update.c light.c panel.c store.c is31fl3731.c ${ARGN}
        ${CORE}/vm.c ${CORE}/sha256.c ${CORE}/tasks.c ${CORE}/token.c ${CORE}/font.c ${CORE}/shell.c ${CORE}/content_gen.c)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    target_compile_definitions(${name} PRIVATE
        MK1_VERSION="${MK1_VERSION}"
        MK1_STORE_SECTORS=${MK1_STORE_SECTORS}
        MK1_E10_OFFSET=${MK1_E10_OFFSET}
        USBD_MANUFACTURER="keetsta"
        USBD_PRODUCT="${product}"
        # a whole screen of the manual fits in the USB buffer instead of trickling out 64 bytes at a time
        CFG_TUD_CDC_TX_BUFSIZE=2048
        CFG_TUD_CDC_RX_BUFSIZE=256
        # one core: flash writes only need this core's interrupts off
        PICO_FLASH_ASSUME_CORE1_SAFE=1
        # all of SCRATCH_Y instead of half: the USB interrupt nests on this stack too
        PICO_STACK_SIZE=0x1000
        # the 1200 baud reboot into BOOTSEL is main.c's: it gives the matrix the update loop first
        PICO_ENABLE_USB_RESET_VIA_BAUD_RATE=0
    )
    target_link_options(${name} PRIVATE "LINKER:-T,${MK1_FLASH_BOUNDS}")
    set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS "${MK1_FLASH_BOUNDS}")
    if (PICO_RP2350_A2_SUPPORTED)
        set_target_properties(${name} PROPERTIES PICOTOOL_EXTRA_UF2_ARGS "--abs-block;${MK1_E10_PAGE}")
    endif()
    target_link_libraries(${name} pico_stdlib pico_flash pico_rand pico_unique_id hardware_adc hardware_flash hardware_i2c hardware_watchdog)
    pico_enable_stdio_usb(${name} 1)
    pico_enable_stdio_uart(${name} 0)
    pico_set_program_name(${name} "${name}")
    pico_set_program_version(${name} "${MK1_VERSION}")
    pico_set_program_description(${name} "${description}")
    pico_set_program_url(${name} "https://keetsta.me/mk1")
    pico_add_extra_outputs(${name})
endfunction()

mk1_firmware(mk1 "mk-1" "keetsta mk-1: a shell over usb-c" own.c)
# the page finds the card by the vendor id alone, so the demo can tell itself apart in the product name
mk1_firmware(mk1-demo "mk-1 demo" "keetsta mk-1 demo: matrix savers and pong" ${DEMO}/own.c ${DEMO}/pong.c)
