/**
 * @file timebase.c
 * @brief Keep SystemCoreClock and the FreeRTOS tick honest across a
 *        performance-mode change (issue #25).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026, Ambiq
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "am_mcu_apollo.h"

#include "timebase.h"

///////////////////////////////////////////////////////////////////////////////
// Mode -> core frequency
///////////////////////////////////////////////////////////////////////////////
//
// WHY A TABLE AND NOT A FREQUENCY GETTER. The first choice was a HAL call
// that returns the live core frequency in Hz: am_hal_clkgen_status_get()
// fills am_hal_clkgen_status_t.ui32SysclkFreq (declared in the apollo510 HAL
// header am_hal_clkgen.h). Its implementation is NOT in this source tree --
// the Apollo510 HAL ships as a prebuilt library here -- so there is no way
// from this repo to confirm that ui32SysclkFreq follows the MCU performance
// mode rather than reporting a fixed value. Rather than build the tick period
// on an unverified return value, this uses the documented MODE query and maps
// the mode to the two operating points the datasheet specifies.
//
// If someone later confirms against the AmbiqSuite source that
// ui32SysclkFreq tracks the performance mode, prefer it: it removes the table
// and would cover the parts left as TODO(verify) below.

#if defined(AM_PART_APOLLO510B)

/* Apollo510B core frequency at each MCU performance mode.
 * Source: Apollo510B SoC Datasheet DS-A510B-1p1p0, Table 39 "Current
 *         Consumption in Active Mode and Sleep Modes", p.216, 2026 -- the
 *         test conditions for IRUNLPFB (low-power mode, 96 MHz) and IRUNHPFB
 *         (high-performance mode, 250 MHz). These are the same two operating
 *         points the battery model's constant pairs are quoted at (see
 *         constants.h) and the pair verified on issue #18's bench runlogs. */
#define TIMEBASE_CORE_CLOCK_LP_HZ (96000000u)
#define TIMEBASE_CORE_CLOCK_HP_HZ (250000000u)
#define TIMEBASE_HAVE_MODE_TABLE  (1)

#else

/* TODO(verify): the core frequency at each MCU performance mode for
 * apollo510_evb (AM_PART_APOLLO510, non-B) and apollo330mP_evb
 * (AM_PART_APOLLO330P) against their own datasheets. The apollo510 HAL header
 * annotates AM_HAL_PWRCTRL_MCU_MODE_HIGH_PERFORMANCE as "192 MHz or 250 MHz"
 * and the apollo330P header splits high performance into HP1 (192 MHz) and
 * HP2 (250 MHz), so neither has a single sourced value here. Until one exists
 * this is a no-op on those boards: SystemCoreClock and the tick keep their
 * current (boot-time) behaviour, i.e. issue #25 is unfixed there, rather than
 * being "fixed" with a number nobody checked. The demo hardware is
 * apollo510b_evb. */

#endif

///////////////////////////////////////////////////////////////////////////////

#if defined(TIMEBASE_HAVE_MODE_TABLE)
/**
 * @brief Core frequency the CPU is running at right now, or 0 if unknown.
 *
 * am_hal_pwrctrl_mcu_mode_status() reports the MCU performance mode the HAL
 * currently has selected (it is the read side of the
 * am_hal_pwrctrl_mcu_mode_select() that nsx_power_set_performance_mode()
 * calls). Returning 0 on anything unexpected leaves the timebase alone, which
 * is the safe direction: a stale-but-consistent tick beats a tick programmed
 * from a frequency we could not read.
 */
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

    /* Before the scheduler starts, setting SystemCoreClock is the whole job.
     * The CM55 port programs SysTick once, in vPortSetupTimerInterrupt()
     * (FreeRTOS-Kernel/portable/GCC/ARM_CM55_NTZ/non_secure/port.c:801), from
     * configSYSTICK_CLOCK_HZ, which defaults to configCPU_CLOCK_HZ (port.c
     * :339) = SystemCoreClock (FreeRTOSConfig.h:49). That call happens inside
     * xPortStartScheduler(), i.e. AFTER this one, so it picks up the value
     * written here and a SysTick write here would only be overwritten.
     * configUSE_TICKLESS_IDLE is 0 (FreeRTOSConfig.h:80), so the port's other
     * SysTick reload writes -- all in the tickless idle path, and all using
     * the reload value cached at scheduler start -- are not compiled in.
     * Nothing re-reads SystemCoreClock after that point, which is why the
     * running case below has to touch the hardware itself. */
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        SystemCoreClock = coreClockHz;
        return;
    }

    /* Scheduler running: SystemCoreClock and the SysTick reload have to move
     * together, or a tick taken between the two is timed on one clock and
     * attributed on the other. Writing VAL clears the current count so the
     * new period starts immediately; the tick straddling the switch is short
     * or long by up to one period, which is the accepted cost of the switch.
     * xTickCount is deliberately untouched -- ticks already counted stay
     * counted, so uptime_ms is continuous across the toggle. */
    const uint32_t reload = (coreClockHz / configTICK_RATE_HZ) - 1u;

    taskENTER_CRITICAL();
    SystemCoreClock = coreClockHz;
    SysTick->LOAD = reload;
    SysTick->VAL = 0u;
    taskEXIT_CRITICAL();
#endif // TIMEBASE_HAVE_MODE_TABLE
}
