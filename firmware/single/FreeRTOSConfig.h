/* FreeRTOS on core 0 of the RP2350 (single-chip build). Core 1 is not under the RTOS: it runs the
   cable's timing-critical path (core1_main in main.c). */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configCPU_CLOCK_HZ                      ( ( unsigned long ) 250000000 )
#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                ( ( unsigned short ) 256 )
#define configMAX_TASK_NAME_LEN                 12
#define configUSE_16_BIT_TICKS                  0
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TASK_NOTIFICATIONS            1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 2 )
#define configTIMER_QUEUE_LENGTH                16
#define configTIMER_TASK_STACK_DEPTH            1024
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0
#define configTOTAL_HEAP_SIZE                   ( 96 * 1024 )
#define configNUMBER_OF_CORES                   1
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    ( 1 << ( 8 - 4 ) )
#define configKERNEL_INTERRUPT_PRIORITY         ( 15 << ( 8 - 4 ) )
#define configPRIO_BITS                         4
#define configUSE_NEWLIB_REENTRANT              0
#define configASSERT( x )                       do { if( ( x ) == 0 ) { for( ;; ) { } } } while( 0 )
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xSemaphoreGetMutexHolder        1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTimerPendFunctionCall          1
#define configUSE_TIMERS                        1
#define xPortPendSVHandler                      isr_pendsv
#define vPortSVCHandler                         isr_svcall
#define xPortSysTickHandler                     isr_systick
/* the Pico SDK's async_context_freertos.c asks the port whether it runs in an interrupt */
#ifndef __ASSEMBLER__
#include <stdint.h>
static inline int dc_in_isr(void) { uint32_t ipsr; __asm volatile ("mrs %0, ipsr" : "=r" (ipsr)); return ipsr != 0; }
#define portCHECK_IF_IN_ISR() dc_in_isr()
#endif
#endif
