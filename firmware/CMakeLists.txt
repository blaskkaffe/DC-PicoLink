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
    hardware_pio hardware_dma hardware_clocks hardware_vreg tinyusb_device tinyusb_board)
if(DC_SYS_KHZ)   # optional overclock: cmake -DDC_SYS_KHZ=250000 ..
    target_compile_definitions(dc_usb_serial PRIVATE DC_SYS_KHZ=${DC_SYS_KHZ})
endif()
if(DEFINED DC_DIAG)   # 0 = leave out the heavy diagnostics (see dcmode.h)
    target_compile_definitions(dc_usb_serial PRIVATE DC_DIAG=${DC_DIAG})
endif()
if(DEFINED W5_VERSIONR_VALUE)   # diagnostics: cmake -DW5_VERSIONR_VALUE=0x08 ..
    target_compile_definitions(dc_usb_serial PRIVATE W5_VERSIONR_VALUE=${W5_VERSIONR_VALUE})
endif()
if(DEFINED DC_POWER_PIN)   # sense wire GPIO (default 7, found by itself); -1 = off
    target_compile_definitions(dc_usb_serial PRIVATE DC_POWER_PIN=${DC_POWER_PIN})
endif()
pico_set_binary_type(dc_usb_serial copy_to_ram)   # no flash-cache stalls on core1
pico_add_extra_outputs(dc_usb_serial)
