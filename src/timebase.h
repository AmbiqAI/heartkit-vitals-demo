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
 * @brief Synchronize SystemCoreClock and the SysTick reload after a mode change.
 * Call after every performance-mode change; safe before scheduler startup.
 * The tick count is preserved. The in-flight tick retains its previous reload
 * at the changed clock rate; do not assume that interval is a full tick period.
 * See AmbiqAI/heartkit-vitals-demo#25.
 */
void timebase_sync_to_core_clock(void);

#ifdef __cplusplus
}
#endif

#endif // HKV_TIMEBASE_H
