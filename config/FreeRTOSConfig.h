/*
 * SPDX-License-Identifier: MIT
 *
 * nsx-freertos Phase 1 reference FreeRTOSConfig.h template.
 *
 * COPY this file into your application (as FreeRTOSConfig.h) and tune it.
 * FreeRTOSConfig.h values are APPLICATION policy: neither nsx-freertos nor the
 * core SDK own heap size, task topology, stack sizes, priorities, or tick rate.
 *
 * Target profile for this template:
 *   - SoC:        staged Cortex-M55 or Cortex-M4F boards
 *   - Port:       generic upstream ARM_CM55_NTZ (non-TrustZone) or ARM_CM4F
 *   - Tick:       plain SysTick (tickless idle disabled)
 *   - Heap:       heap_4 (configTOTAL_HEAP_SIZE below)
 *
 * Handler binding: ARM_CM55_NTZ provides strong SVC_Handler / PendSV_Handler /
 * SysTick_Handler symbols directly. ARM_CM4F uses strong nsx-freertos shim
 * handlers that route those vectors to FreeRTOS' vPortSVCHandler /
 * xPortPendSVHandler / xPortSysTickHandler symbols. Do NOT remap or redefine
 * those handlers in the application.
 *
 * See https://www.FreeRTOS.org/a00110.html for the full option reference.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;

/* Shared generic Cortex-M port configuration. ARMv8-M-only knobs below are
 * harmless when the selected port is ARM_CM4F and keep one shared template. */
#define configENABLE_TRUSTZONE                          0
#define configRUN_FREERTOS_SECURE_ONLY                  1
#define configENABLE_MPU                                0
#define configENABLE_FPU                                1
#define configENABLE_MVE                                1
#define configTOTAL_MPU_REGIONS                         16

/* Scheduler behaviour. */
#define configUSE_PREEMPTION                            1
#define configUSE_TIME_SLICING                          1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION         0
#define configMAX_PRIORITIES                            ( 8 )
#define configIDLE_SHOULD_YIELD                         1
#define configUSE_16_BIT_TICKS                          0

/* Clocking and memory (application policy). */
#define configCPU_CLOCK_HZ                              ( SystemCoreClock )
#define configTICK_RATE_HZ                              ( ( TickType_t ) 1000 )
#define configMINIMAL_STACK_SIZE                        ( ( uint16_t ) 256 )
#define configMINIMAL_SECURE_STACK_SIZE                 ( 1024 )
#define configMAX_TASK_NAME_LEN                         ( 16 )
/* Bumped from the phase-1 scaffold default of 32 KiB: phase 6 adds
 * CpuProcessTask + TioProcessTask (2 more task stacks) plus a 32-entry x
 * 256B TileIO TX queue (8 KiB) sized to match legacy's queue depth --
 * heap_4 must cover all task stacks + TCBs + the queue's own storage,
 * which alone exceeds 32 KiB. Sized with headroom on top of the ~41 KiB
 * hard minimum (6 task stacks ~32 KiB + TIO queue 8 KiB + TCB/idle/timer
 * overhead ~3 KiB). Malloc-failed-hook fires immediately at boot
 * (xQueueCreate for the TIO queue) if this is too small -- confirmed via
 * SWO (neuralspotx PR #175 / nsx 0.7.4) after this heap bump.
 *
 * apollo510b_evb only: TileIO BLE (ble_bringup.c) adds a 7th task
 * (BleRadioTask, 4096-word/16 KiB stack -- matches the ble_webble
 * reference example's radio task sizing) on top of the 48 KiB baseline
 * above, which is otherwise sufficient for the other 2 boards (no BLE
 * task there -- ble_bringup.c is compiled out, see CMakeLists.txt).
 * 48 KiB + ~16 KiB task stack + TCB overhead rounds up to 72 KiB with
 * headroom. Reproduced + confirmed via SWO: without this bump,
 * xTaskCreate(BleRadioTask, ...) exhausts the heap and
 * vApplicationMallocFailedHook() fires at boot on apollo510b_evb. */
#if defined(AM_PART_APOLLO510B)
#define configTOTAL_HEAP_SIZE                           ( ( size_t ) ( 72 * 1024 ) )
#else
#define configTOTAL_HEAP_SIZE                           ( ( size_t ) ( 48 * 1024 ) )
#endif

/* Phase 1 uses a plain SysTick tick; tickless idle is disabled. */
#define configUSE_TICKLESS_IDLE                         0

/* Feature selection. */
#define configUSE_MUTEXES                               1
#define configUSE_RECURSIVE_MUTEXES                     1
#define configUSE_COUNTING_SEMAPHORES                   1
#define configUSE_TASK_NOTIFICATIONS                    1
#define configUSE_QUEUE_SETS                            0
/* Runtime stats: app-level CpuProcessTask (src/main.cc) reads
 * per-task run-time counters via uxTaskGetSystemState(), which requires
 * configGENERATE_RUN_TIME_STATS + configUSE_TRACE_FACILITY. The app
 * supplies the timer hooks below (RTOS_AppConfigureTimerForRuntimeStats /
 * RTOS_AppGetRuntimeCounterValueFromISR), backed by an am_hal_timer
 * instance (RTOS_TIMER, see constants.h). */
#define configGENERATE_RUN_TIME_STATS                   1
#define configUSE_TRACE_FACILITY                        1
#define configUSE_STATS_FORMATTING_FUNCTIONS             0
#define configQUEUE_REGISTRY_SIZE                       0
#define configUSE_NEWLIB_REENTRANT                      0

/* Hook functions. */
#define configUSE_IDLE_HOOK                             0
#define configUSE_TICK_HOOK                             0
#define configUSE_MALLOC_FAILED_HOOK                    1
#define configCHECK_FOR_STACK_OVERFLOW                  2

/* Memory allocation scheme. */
#define configSUPPORT_STATIC_ALLOCATION                 0
#define configSUPPORT_DYNAMIC_ALLOCATION                1

/* Software timers. */
#define configUSE_TIMERS                                1
#define configTIMER_TASK_PRIORITY                       ( 3 )
#define configTIMER_QUEUE_LENGTH                         5
#define configTIMER_TASK_STACK_DEPTH                    ( configMINIMAL_STACK_SIZE )

/* Assert. */
#define configASSERT( x )  if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ;; ); }

/* Indirect handler routing. The ARM_CM4F port exposes vPortSVCHandler /
 * xPortPendSVHandler / xPortSysTickHandler rather than the SVC_Handler /
 * PendSV_Handler / SysTick_Handler vector names owned by NSX startup, so
 * nsx-freertos installs strong shim handlers that branch to the port entry
 * points. Disable the direct-installation check accordingly. */
#define configCHECK_HANDLER_INSTALLATION                0

/* Interrupt priority configuration. */
#ifdef __NVIC_PRIO_BITS
    #define configPRIO_BITS                             __NVIC_PRIO_BITS
#elif defined(ARMCM4)
    #define configPRIO_BITS                             3
#else
    #define configPRIO_BITS                             4
#endif
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         ( ( 1U << configPRIO_BITS ) - 1U )
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    3
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )

/* Optional API inclusion. */
#define INCLUDE_vTaskPrioritySet                        1
#define INCLUDE_uxTaskPriorityGet                       1
#define INCLUDE_vTaskDelete                             1
#define INCLUDE_vTaskSuspend                            1
#define INCLUDE_vTaskDelayUntil                         1
#define INCLUDE_vTaskDelay                              1
#define INCLUDE_xTaskGetSchedulerState                  1
#define INCLUDE_xTaskGetCurrentTaskHandle               1
#define INCLUDE_uxTaskGetStackHighWaterMark             1
#define INCLUDE_xTaskGetIdleTaskHandle                  1
#define INCLUDE_eTaskGetState                           1
#define INCLUDE_xTimerPendFunctionCall                  1

/* Run-time stats timer hooks (app-supplied, see src/main.cc). Must
 * be declared before use here since FreeRTOSConfig.h is included ahead of
 * the app's own headers. This header is included from both plain-C
 * FreeRTOS sources and main.cc (C++, since heliaRT/TFLM types force main
 * to be C++) -- guard with extern "C" so the two see matching linkage. */
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t RTOS_AppConfigureTimerForRuntimeStats(void);
extern uint32_t RTOS_AppGetRuntimeCounterValueFromISR(void);
#ifdef __cplusplus
}
#endif
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() RTOS_AppConfigureTimerForRuntimeStats()
#define portGET_RUN_TIME_COUNTER_VALUE() RTOS_AppGetRuntimeCounterValueFromISR()

#endif /* FREERTOS_CONFIG_H */
