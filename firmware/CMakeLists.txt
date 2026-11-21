cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)
project(dc_picolink C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# Version shown in the first log line: git describe (e.g. v0.8.0-3-gabc1234), or -DDC_VERSION=...
if(NOT DC_VERSION)
    execute_process(COMMAND git describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE DC_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT DC_VERSION)
        set(DC_VERSION "unknown")
    endif()
endif()
message(STATUS "DC-PicoLink firmware version ${DC_VERSION}")

add_executable(dc_usb_serial main.c spi_slave.c usb_descriptors.c)
target_compile_definitions(dc_usb_serial PRIVATE DC_VERSION="${DC_VERSION}")
pico_generate_pio_header(dc_usb_serial ${CMAKE_CURRENT_LIST_DIR}/uart.pio)
pico_generate_pio_header(dc_usb_serial ${CMAKE_CURRENT_LIST_DIR}/w5500_spi.pio)
pico_generate_pio_header(dc_usb_serial ${CMAKE_CURRENT_LIST_DIR}/sd_spi.pio)
target_include_directories(dc_usb_serial PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(dc_usb_serial PRIVATE
    PICO_USE_FASTEST_SUPPORTED_CLOCK=1)      # 200 MHz: more headroom for SPI replies
target_link_libraries(dc_usb_serial PRIVATE pico_stdlib pico_unique_id pico_multicore
    hardware_pio hardware_dma hardware_clocks hardware_vreg hardware_spi tinyusb_device tinyusb_board)
if(DC_SYS_KHZ)   # optional overclock: cmake -DDC_SYS_KHZ=250000 ..
    target_compile_definitions(dc_usb_serial PRIVATE DC_SYS_KHZ=${DC_SYS_KHZ})
endif()
if(DEFINED DC_DIAG)   # 0 = leave out the heavy diagnostics (see dcmode.h)
    target_compile_definitions(dc_usb_serial PRIVATE DC_DIAG=${DC_DIAG})
endif()
if(DEFINED W5_VERSIONR_VALUE)   # diagnostics: cmake -DW5_VERSIONR_VALUE=0x08 ..
    target_compile_definitions(dc_usb_serial PRIVATE W5_VERSIONR_VALUE=${W5_VERSIONR_VALUE})
endif()
# Board layout: pico (default: Pico / Pro Micro, Dreamcast on GPIO2-7, ESP32 link on UART0
# GPIO12-15) or zero-stack (RP2040-Zero back to back with an ESP32-S3 SuperMini: Dreamcast
# on GPIO9-14, ESP32 link over SPI0 on GPIO4-7 + GPIO2/3; see hardware/ESP32.md)
if(NOT DC_BOARD)
    set(DC_BOARD pico)
endif()
if(DC_BOARD STREQUAL "zero-stack")
    target_compile_definitions(dc_usb_serial PRIVATE DC_PIN_BASE=9 DC_LINK_SPI=1)
    target_link_libraries(dc_usb_serial PRIVATE hardware_spi)
    if(DEFINED LINK_SPI_HZ)   # SPI link clock request (default 16 MHz -> 15.6 MHz at 250 MHz)
        target_compile_definitions(dc_usb_serial PRIVATE LINK_SPI_HZ=${LINK_SPI_HZ}u)
    endif()
elseif(NOT DC_BOARD STREQUAL "pico")
    message(FATAL_ERROR "DC_BOARD must be pico or zero-stack")
endif()
message(STATUS "DC-PicoLink board layout: ${DC_BOARD}")
if(DEFINED DC_POWER_PIN)   # sense wire GPIO (default: GPIO7, or GPIO29 on the zero-stack; found by itself); -1 = off
    target_compile_definitions(dc_usb_serial PRIVATE DC_POWER_PIN=${DC_POWER_PIN})
endif()
pico_set_binary_type(dc_usb_serial copy_to_ram)   # no flash-cache stalls on core1
pico_enable_stdio_uart(dc_usb_serial 0)   # UART0 is the ESP32 link on the pico layout
pico_enable_stdio_usb(dc_usb_serial 0)
pico_add_extra_outputs(dc_usb_serial)
