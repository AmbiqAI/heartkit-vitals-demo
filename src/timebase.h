// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file timebase.h
 * @brief Keep SystemCoreClock and the FreeRTOS tick honest across a
 *        performance-mode change.
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
 * Does NOT touch `xTickCount`: ticks already counted stay counted. Only the
 * SysTick reload is rewritten, so the one tick in flight finishes at the OLD
 * reload counted at the NEW clock. Switching to high performance it is short
 * (96000 counts at 250 MHz, about 0.38 ms); switching to low power it is long
 * (250000 counts at 96 MHz, about 2.6 ms). Every tick after it is 1 ms again.
 * Do not size a timeout on "one period" for the high-to-low case.
 *
 * Safe to call before `vTaskStartScheduler()`.
 */
void timebase_sync_to_core_clock(void);

#ifdef __cplusplus
}
#endif

#endif // HKV_TIMEBASE_H
