// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#include "am_bsp.h"
#include "am_mcu_apollo.h"
#include "nsx_core.h"
#include "nsx_power.h"

static void report_forever(const char *message)
{
    am_hal_gpio_state_write(0, AM_HAL_GPIO_OUTPUT_CLEAR);
    nsx_itm_printf_enable();
    while (1) {
        nsx_printf("HKV|sleep|%s\r\n", message);
        nsx_delay_us(1000000);
    }
}

int main(void)
{
    nsx_core_config_t core = {.api = &nsx_core_V1_0_0};
    if (nsx_core_init(&core)) { while (1) {} }
    nsx_itm_printf_enable();
    nsx_power_config_t power = nsx_power_minimal;
    power.perf_mode = NSX_POWER_PERF_LOW;
    /* Hold memory policy equal to the prior light-sleep capture; see #68. */
    power.need_ssram = true;
    power.small_tcm = false;
    power.need_itm = true;
    power.spotmgr_collapse = false;
    if (nsx_power_configure(&power)) { report_forever("POWER_INIT_FAILED"); }
    SystemCoreClock = AM_HAL_CLKGEN_FREQ_MAX_HZ;
    if (am_hal_gpio_pinconfig(0, am_hal_gpio_pincfg_output)) {
        report_forever("GATE_INIT_FAILED");
    }
    am_hal_gpio_state_write(0, AM_HAL_GPIO_OUTPUT_CLEAR);
    am_hal_gpio_state_write(AM_BSP_GPIO_EM9305_EN, AM_HAL_GPIO_OUTPUT_CLEAR);
    if (am_hal_gpio_pinconfig(AM_BSP_GPIO_EM9305_EN, g_AM_BSP_GPIO_EM9305_EN) ||
        am_hal_pwrctrl_periph_disable(AM_BSP_EM9305_IOM)) {
        report_forever("RADIO_DISABLE_FAILED");
    }
    for (unsigned remaining = 20; remaining > 0; remaining--) {
        nsx_printf("HKV|sleep|LP entering_light_sleep_in=%u no_timer_wake\r\n", remaining);
        nsx_delay_us(1000000);
    }
    nsx_printf("HKV|sleep|Entering sleep\r\n");
    nsx_delay_us(100000);
    SysTick->CTRL = 0;
    am_hal_stimer_int_disable(0xFFFFFFFFu);
    am_hal_stimer_int_clear(0xFFFFFFFFu);
    am_hal_stimer_config(AM_HAL_STIMER_NO_CLK);
    __disable_irq();
    unsigned banks = sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]);
    for (unsigned i = 0; i < banks; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    am_bsp_itm_printf_disable();
    nsx_power_disable_debug();
    am_hal_pwrctrl_periph_disable(AM_HAL_PWRCTRL_PERIPH_DEBUG);
    am_hal_gpio_state_write(0, AM_HAL_GPIO_OUTPUT_SET);
    __DSB();
    am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_NORMAL);
    report_forever("UNEXPECTED_WAKE");
}
