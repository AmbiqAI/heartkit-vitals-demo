/**
 * @file timebase.h
 * @brief Keep SystemCoreClock and the FreeRTOS tick honest across a
 *        performance-mode change.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, Ambiq
 */

#ifndef HKV_TIMEBASE_H
#define HKV_TIMEBASE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Re-point the software timebase at the core clock the CPU is
 *        ACTUALLY running at, and hold the FreeRTOS tick period at 1 ms.
 *
 * Call this after EVERY performance-mode change (boot and the dashboard
 * speed toggle). Nothing in the CMSIS system file, the AmbiqSuite HAL or
 * nsx-power updates `SystemCoreClock` when the MCU performance mode changes,
 * so without this call the whole software timebase stays calibrated for the
 * low-power clock: the FreeRTOS tick runs fast by the clock ratio and every
 * DWT-measured duration is over-reported by the same factor. See issue #25.
 *
 * Does NOT touch `xTickCount`: ticks already counted stay counted. The one
 * tick straddling the switch is short or long by up to one period; every tick
 * after it is 1 ms again.
 *
 * Safe to call before `vTaskStartScheduler()`.
 */
void timebase_sync_to_core_clock(void);

#ifdef __cplusplus
}
#endif

#endif // HKV_TIMEBASE_H
