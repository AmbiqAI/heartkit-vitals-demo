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

#if defined(AM_PART_APOLLO510B) || defined(AM_PART_APOLLO510)

/* Core frequency at each MCU performance mode, for BOTH Apollo510 parts
 * (apollo510b_evb and apollo510_evb compile against the same apollo510 HAL).
 * Taken from the HAL's own macros rather than literals so the values follow
 * the build configuration -- note AM_HAL_CLKGEN_FREQ_MAX_HZ is redefined for
 * APOLLO5_FPGA builds (am_hal_clkgen.h:38-40), which a literal would get
 * wrong.
 * Source, low power: am_hal_clkgen.h:43, AM_HAL_CLKGEN_FREQ_MAX_HZ 96000000.
 * Source, high performance: am_hal_clkgen.h:47, AM_HAL_CLKGEN_FREQ_HP250_HZ
 *         250000000, with the note at :45 "CAYNSWS-1744 Apollo510 remove
 *         CPUHPFREQSEL, the only valid HP frequency is 250MHz" -- i.e. the
 *         "192 MHz or 250 MHz" annotation on
 *         AM_HAL_PWRCTRL_MCU_MODE_HIGH_PERFORMANCE (am_hal_pwrctrl.h:213) is
 *         stale for this part; there is only one HP frequency.
 * Corroborated for Apollo510B by the Apollo510B SoC Datasheet DS-A510B-1p1p0,
 *         Table 39 "Current Consumption in Active Mode and Sleep Modes",
 *         p.216, 2026 -- the test conditions for IRUNLPFB (low-power mode,
 *         96 MHz) and IRUNHPFB (high-performance mode, 250 MHz). Those are the
 *         same two operating points the battery model's constant pairs are
 *         quoted at (see constants.h) and the pair verified on issue #18's
 *         bench runlogs.
 *
 * This guard must stay AT LEAST as wide as the set of boards where anything
 * else follows appState.speedMode. inference_power_mw() in main.cc divides by
 * the HP power figure in HP mode on every board; if the timebase were not
 * fixed here too, that board would divide an under-reported IPS by the HP
 * power and read worse than before the fix. */
#define TIMEBASE_CORE_CLOCK_LP_HZ ((uint32_t)AM_HAL_CLKGEN_FREQ_MAX_HZ)
#define TIMEBASE_CORE_CLOCK_HP_HZ ((uint32_t)AM_HAL_CLKGEN_FREQ_HP250_HZ)
#define TIMEBASE_HAVE_MODE_TABLE  (1)

#else

/* TODO(verify): the core frequency at each MCU performance mode for
 * apollo330mP_evb (AM_PART_APOLLO330P) against its own datasheet. That part's
 * HAL splits high performance into HP1 (192 MHz) and HP2 (250 MHz)
 * (am_hal_pwrctrl.h:218-220) and this repo has no sourced value for which one
 * NSX_POWER_PERF_HIGH lands on, so this is a no-op there: SystemCoreClock and
 * the tick keep their current (boot-time) behaviour, i.e. issue #25 is unfixed
 * on that board, rather than being "fixed" with a number nobody checked.
 * Nothing else on that board follows the operating point either -- its
 * MCU_INFERENCE_POWER_MW_LP and _HP are the same figure (constants.h:197-198),
 * so inference_power_mw() is mode-independent there. The demo hardware is
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
     * attributed on the other.
     *
     * LOAD ONLY -- DO NOT ALSO WRITE SysTick->VAL. Writing LOAD alone lets the
     * period already in flight finish at its old length; the new reload is
     * latched at the next wrap. That bounds the disturbance to exactly one
     * tick, however often this is called. Writing VAL would restart the count
     * from zero, so a host toggling speed_mode on successive UIO frames
     * (TioProcessTask services one per tick) could keep resetting the counter
     * before it ever wraps and starve the tick entirely, drifting uptime_ms.
     * The one-tick error is the accepted cost of a mode switch; an unbounded
     * one is not.
     *
     * xTickCount is deliberately untouched -- ticks already counted stay
     * counted, so uptime_ms is continuous across the toggle. */
    const uint32_t reload = (coreClockHz / configTICK_RATE_HZ) - 1u;

    taskENTER_CRITICAL();
    SystemCoreClock = coreClockHz;
    SysTick->LOAD = reload;
    taskEXIT_CRITICAL();
#endif // TIMEBASE_HAVE_MODE_TABLE
}
