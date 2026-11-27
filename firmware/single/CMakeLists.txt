# Single-chip build: the cable (core 1 timing path, core 0 USB/serial loop) plus the network side
# (Wi-Fi, lwIP, PPP, ...) under FreeRTOS on the same RP2350 W. cmake -DDC_SINGLE=ON -DPICO_BOARD=pico2_w
if(NOT DEFINED FREERTOS_KERNEL_PATH)
    set(FREERTOS_KERNEL_PATH $ENV{FREERTOS_KERNEL_PATH})
endif()
if(NOT FREERTOS_KERNEL_PATH OR NOT EXISTS ${FREERTOS_KERNEL_PATH}/tasks.c)
    message(FATAL_ERROR "DC_SINGLE needs FreeRTOS-Kernel: -DFREERTOS_KERNEL_PATH=/path/to/FreeRTOS-Kernel")
endif()
set(FRP ${FREERTOS_KERNEL_PATH}/portable/GCC/ARM_CM33_NTZ/non_secure)
add_library(FreeRTOS-Kernel INTERFACE)
target_sources(FreeRTOS-Kernel INTERFACE
    ${FREERTOS_KERNEL_PATH}/event_groups.c ${FREERTOS_KERNEL_PATH}/list.c ${FREERTOS_KERNEL_PATH}/queue.c
    ${FREERTOS_KERNEL_PATH}/stream_buffer.c ${FREERTOS_KERNEL_PATH}/tasks.c ${FREERTOS_KERNEL_PATH}/timers.c
    ${FREERTOS_KERNEL_PATH}/portable/MemMang/heap_4.c
    ${FRP}/port.c ${FRP}/portasm.c ${FRP}/mpu_wrappers_v2_asm.c)
target_include_directories(FreeRTOS-Kernel INTERFACE
    ${FREERTOS_KERNEL_PATH}/include ${FRP} ${FREERTOS_KERNEL_PATH}/portable/GCC/ARM_CM33_NTZ/non_secure
    ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(FreeRTOS-Kernel INTERFACE pico_base_headers hardware_clocks hardware_exception)
add_library(FreeRTOS-Kernel-Heap4 INTERFACE)
target_link_libraries(FreeRTOS-Kernel-Heap4 INTERFACE FreeRTOS-Kernel)

add_executable(dc_net_probe net_probe.c)
target_include_directories(dc_net_probe PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(dc_net_probe PRIVATE PICO_CYW43_ARCH_FREERTOS=1)
target_link_libraries(dc_net_probe PRIVATE pico_stdlib pico_cyw43_arch_lwip_sys_freertos FreeRTOS-Kernel FreeRTOS-Kernel-Heap4)
pico_add_extra_outputs(dc_net_probe)
