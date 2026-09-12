// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file timebase.c
 * @brief Keep SystemCoreClock and the FreeRTOS tick honest across a
 *        performance-mode change (issue #25).
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "am_mcu_apollo.h"

#include "timebase.h"

// Resolve performance modes through HAL clocks; see AmbiqAI/heartkit-vitals-demo#25.

#if defined(AM_PART_APOLLO510B) || defined(AM_PART_APOLLO510)

/* HAL frequency macros preserve platform overrides; see AmbiqAI/heartkit-vitals-demo#25. */
#define TIMEBASE_CORE_CLOCK_LP_HZ ((uint32_t)AM_HAL_CLKGEN_FREQ_MAX_HZ)
#define TIMEBASE_CORE_CLOCK_HP_HZ ((uint32_t)AM_HAL_CLKGEN_FREQ_HP250_HZ)
#define TIMEBASE_HAVE_MODE_TABLE  (1)

#else

/* TODO(#71): verify the Apollo330 high-performance clock mapping. */

#endif

///////////////////////////////////////////////////////////////////////////////

#if defined(TIMEBASE_HAVE_MODE_TABLE)
/** @brief Return the selected core frequency, or zero when it cannot be resolved. */
static uint32_t
timebase_core_clock_hz(void)
{
    am_hal_pwrctrl_mcu_mode_e eMode;

    if (am_hal_pwrctrl_mcu_mode_status(&eMode) != AM_HAL_STATUS_SUCCESS) {
        return 0u;
    }

    switch (eMode) {
    case AM_HAL_PWRCTRL_MCU_MODE_LOW_POWER:
        return TIMEBASE_CORE_CLOCK_LP_HZ;
    case AM_HAL_PWRCTRL_MCU_MODE_HIGH_PERFORMANCE:
        return TIMEBASE_CORE_CLOCK_HP_HZ;
    default:
        return 0u;
    }
}
#endif // TIMEBASE_HAVE_MODE_TABLE

void
timebase_sync_to_core_clock(void)
{
#if defined(TIMEBASE_HAVE_MODE_TABLE)
    const uint32_t coreClockHz = timebase_core_clock_hz();

    if (coreClockHz == 0u) {
        return;
    }

    /* Scheduler startup configures SysTick from SystemCoreClock. */
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        SystemCoreClock = coreClockHz;
        return;
    }

    /* Update the reload atomically with SystemCoreClock. Leave VAL and xTickCount
     * intact to avoid starving ticks during repeated mode switches. */
    const uint32_t reload = (coreClockHz / configTICK_RATE_HZ) - 1u;

    taskENTER_CRITICAL();
    SystemCoreClock = coreClockHz;
    SysTick->LOAD = reload;
    taskEXIT_CRITICAL();
#endif // TIMEBASE_HAVE_MODE_TABLE
}
