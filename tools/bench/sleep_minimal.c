// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#include "am_bsp.h"
#include "am_mcu_apollo.h"
#include "nsx_core.h"
#include "nsx_power.h"

typedef struct {
    uint32_t stage;
    uint32_t cpu_power;
    uint32_t scr;
    uint32_t peripheral_enable;
    uint32_t peripheral_status;
    uint32_t memory_enable;
    uint32_t memory_status;
    uint32_t memory_retention;
    uint32_t sram_enable;
    uint32_t sram_status;
    uint32_t sram_retention;
    uint32_t regulator_status;
    uint32_t mram_control;
    uint32_t oscillator_control;
    uint32_t clock_status;
    uint32_t system_power_status;
    uint32_t ble_enable_output;
    uint32_t ble_enable_input;
} sleep_snapshot_t;

volatile sleep_snapshot_t g_sleep_snapshot;
volatile uint32_t g_sleep_wake_status;
static volatile bool sleep_armed;

static void snapshot(uint32_t stage)
{
    g_sleep_snapshot.cpu_power = PWRCTRL->CPUPWRCTRL;
    g_sleep_snapshot.scr = SCB->SCR;
    g_sleep_snapshot.peripheral_enable = PWRCTRL->DEVPWREN;
    g_sleep_snapshot.peripheral_status = PWRCTRL->DEVPWRSTATUS;
    g_sleep_snapshot.memory_enable = PWRCTRL->MEMPWREN;
    g_sleep_snapshot.memory_status = PWRCTRL->MEMPWRSTATUS;
    g_sleep_snapshot.memory_retention = PWRCTRL->MEMRETCFG;
    g_sleep_snapshot.sram_enable = PWRCTRL->SSRAMPWREN;
    g_sleep_snapshot.sram_status = PWRCTRL->SSRAMPWRST;
    g_sleep_snapshot.sram_retention = PWRCTRL->SSRAMRETCFG;
    g_sleep_snapshot.regulator_status = PWRCTRL->VRSTATUS;
    g_sleep_snapshot.mram_control = MCUCTRL->MRAMCRYPTOPWRCTRL;
    g_sleep_snapshot.oscillator_control = CLKGEN->OCTRL;
    g_sleep_snapshot.clock_status = CLKGEN->CLOCKENSTAT;
    g_sleep_snapshot.system_power_status = PWRCTRL->SYSPWRSTATUS;
    g_sleep_snapshot.ble_enable_output = am_hal_gpio_output_read(AM_BSP_GPIO_EM9305_EN);
    g_sleep_snapshot.ble_enable_input = am_hal_gpio_input_read(AM_BSP_GPIO_EM9305_EN);
    g_sleep_snapshot.stage = stage;
}

/* This HAL hook is called after sleep preparation, immediately before WFI.
 * A marker outside the HAL cannot distinguish a preparation stall. See #68. */
void am_hal_PRE_SLEEP_PROCESSING(void)
{
    if (sleep_armed) {
        snapshot(2);
        am_hal_gpio_state_write(0, AM_HAL_GPIO_OUTPUT_SET);
        __DSB();
    }
}

static void fail(const char *message, uint32_t status)
{
    sleep_armed = false;
    am_hal_gpio_state_write(0, AM_HAL_GPIO_OUTPUT_CLEAR);
    am_hal_pwrctrl_periph_enable(AM_HAL_PWRCTRL_PERIPH_DEBUG);
    nsx_itm_printf_enable();
    while (1) {
        nsx_printf("HKV|minsleep|%s status=%lu stage=%lu\r\n", message,
                   (unsigned long)status, (unsigned long)g_sleep_snapshot.stage);
        nsx_delay_us(1000000);
    }
}

static void require_ok(const char *operation, uint32_t status)
{
    if (status != AM_HAL_STATUS_SUCCESS) { fail(operation, status); }
}

static void disable_peripheral(am_hal_pwrctrl_periph_e peripheral)
{
    bool enabled = true;
    require_ok("PERIPHERAL_STATUS_FAILED", am_hal_pwrctrl_periph_enabled(peripheral, &enabled));
    if (enabled) {
        require_ok("PERIPHERAL_DISABLE_FAILED", am_hal_pwrctrl_periph_disable(peripheral));
    }
    require_ok("PERIPHERAL_READBACK_FAILED", am_hal_pwrctrl_periph_enabled(peripheral, &enabled));
    if (enabled) { fail("PERIPHERAL_STILL_ON", peripheral); }
}

int main(void)
{
    nsx_core_config_t core = {.api = &nsx_core_V1_0_0};
    if (nsx_core_init(&core)) { while (1) {} }
    nsx_itm_printf_enable();
    require_ok("GATE_INIT_FAILED", am_hal_gpio_pinconfig(0, am_hal_gpio_pincfg_output));
    am_hal_gpio_state_write(0, AM_HAL_GPIO_OUTPUT_CLEAR);

    nsx_power_config_t power = nsx_power_minimal;
    power.perf_mode = NSX_POWER_PERF_LOW;
    power.small_tcm = true;
    power.need_ssram = false;
    power.need_itm = true;
    power.spotmgr_collapse = false;
    require_ok("POWER_INIT_FAILED", nsx_power_configure(&power));
    SystemCoreClock = AM_HAL_CLKGEN_FREQ_MAX_HZ;
    const am_hal_pwrctrl_dtcm_select_e tcm =
        HKV_SLEEP_TCM_KIB == 160 ? AM_HAL_PWRCTRL_ITCM32K_DTCM128K :
        HKV_SLEEP_TCM_KIB == 384 ? AM_HAL_PWRCTRL_ITCM128K_DTCM256K :
                                 AM_HAL_PWRCTRL_ITCM256K_DTCM512K;
    const am_hal_pwrctrl_sram_select_e sram_banks =
        HKV_SLEEP_SRAM_MIB == 0 ? AM_HAL_PWRCTRL_SRAM_NONE :
        HKV_SLEEP_SRAM_MIB == 1 ? AM_HAL_PWRCTRL_SRAM_1M :
        HKV_SLEEP_SRAM_MIB == 2 ? AM_HAL_PWRCTRL_SRAM_2M : AM_HAL_PWRCTRL_SRAM_3M;
    am_hal_pwrctrl_mcu_memory_config_t memory = {
        .eROMMode = AM_HAL_PWRCTRL_ROM_AUTO,
        .eDTCMCfg = tcm,
        .eRetainDTCM = AM_HAL_PWRCTRL_MEMRETCFG_TCMPWDSLP_RETAIN,
        .eNVMCfg = HKV_SLEEP_SINGLE_MRAM ? AM_HAL_PWRCTRL_NVM0_ONLY : AM_HAL_PWRCTRL_NVM0_AND_NVM1,
        .bKeepNVMOnInDeepSleep = false,
    };
    am_hal_pwrctrl_sram_memcfg_t sram = {
        .eSRAMCfg = sram_banks,
        .eActiveWithMCU = AM_HAL_PWRCTRL_SRAM_NONE,
        .eActiveWithGFX = AM_HAL_PWRCTRL_SRAM_NONE,
        .eActiveWithDISP = AM_HAL_PWRCTRL_SRAM_NONE,
        .eSRAMRetain = sram_banks,
    };
    require_ok("TCM_CONFIG_FAILED", am_hal_pwrctrl_mcu_memory_config(&memory));
#if HKV_SLEEP_MRAM_LOW_POWER_READ
    /* Match nsx_power_minimize_memory without putting executing MRAM to sleep;
     * mode bits must be valid before enabling their software override. See #68. */
    MCUCTRL->MRAMCRYPTOPWRCTRL_b.MRAM0LPREN = 1;
    MCUCTRL->MRAMCRYPTOPWRCTRL_b.MRAM0SLPEN = 0;
    MCUCTRL->MRAMCRYPTOPWRCTRL_b.MRAM0PWRCTRL = 1;
    __DSB();
    __ISB();
#endif
    require_ok("SRAM_CONFIG_FAILED", am_hal_pwrctrl_sram_config(&sram));
    if (PWRCTRL->MEMPWRSTATUS_b.PWRSTTCM != tcm ||
        PWRCTRL->SSRAMPWRST != sram_banks) {
        fail("MEMORY_READBACK_MISMATCH", PWRCTRL->MEMPWRSTATUS);
    }
    am_hal_gpio_state_write(AM_BSP_GPIO_EM9305_EN, AM_HAL_GPIO_OUTPUT_CLEAR);
    /* Input sensing must be enabled to verify the disable level at the pad;
     * an output-latch read alone cannot detect contention. See #68. */
    am_hal_gpio_pincfg_t radio_enable = g_AM_BSP_GPIO_EM9305_EN;
    radio_enable.GP.cfg_b.eGPInput = AM_HAL_GPIO_PIN_INPUT_ENABLE;
    radio_enable.GP.cfg_b.eGPRdZero = AM_HAL_GPIO_PIN_RDZERO_READPIN;
    require_ok("RADIO_PIN_FAILED", am_hal_gpio_pinconfig(AM_BSP_GPIO_EM9305_EN,
                                                       radio_enable));
    require_ok("RADIO_CLOCK_PIN_FAILED", am_hal_gpio_pinconfig(AM_BSP_GPIO_EM9305_32K_CLK,
                                                              am_hal_gpio_pincfg_disabled));
    if (am_hal_gpio_output_read(AM_BSP_GPIO_EM9305_EN) ||
        am_hal_gpio_input_read(AM_BSP_GPIO_EM9305_EN)) { fail("RADIO_DISABLE_LEVEL_FAILED", 1); }
    for (unsigned p = 0; p < AM_HAL_PWRCTRL_PERIPH_MAX; p++) {
        if (p != AM_HAL_PWRCTRL_PERIPH_DEBUG) { disable_peripheral(p); }
    }
    for (unsigned t = 0; t < 16; t++) { am_hal_timer_stop(t); }
    PWRCTRL->CPUPWRCTRL_b.SLEEPMODE = PWRCTRL_CPUPWRCTRL_SLEEPMODE_AMBIQ_SLEEP;
    snapshot(1);
    nsx_printf("HKV|minsleep|LP normal_WFI TCM_KiB=%u SSRAM_MiB=%u\r\n",
               HKV_SLEEP_TCM_KIB, HKV_SLEEP_SRAM_MIB);
    nsx_printf("HKV|minsleep|single_mram=%u low_power_read=%u\r\n",
               HKV_SLEEP_SINGLE_MRAM, HKV_SLEEP_MRAM_LOW_POWER_READ);
    nsx_printf("HKV|minsleep|cpu=%08lx mem=%08lx sram=%08lx vr=%08lx\r\n",
               (unsigned long)g_sleep_snapshot.cpu_power,
               (unsigned long)g_sleep_snapshot.memory_enable,
               (unsigned long)g_sleep_snapshot.sram_status,
               (unsigned long)g_sleep_snapshot.regulator_status);
    for (unsigned remaining = 20; remaining > 0; remaining--) {
        nsx_printf("HKV|minsleep|sleep_in=%u detach_debugger\r\n", remaining);
        nsx_delay_us(1000000);
    }
    nsx_printf("HKV|minsleep|Entering sleep; no scheduled wake\r\n");
    nsx_delay_us(100000);
    SysTick->CTRL = 0;
    am_hal_stimer_int_disable(0xFFFFFFFFu);
    am_hal_stimer_int_clear(0xFFFFFFFFu);
    am_hal_stimer_config(AM_HAL_STIMER_NO_CLK);
    __disable_irq();
    for (unsigned i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    am_bsp_itm_printf_disable();
    nsx_power_disable_debug();
    disable_peripheral(AM_HAL_PWRCTRL_PERIPH_DEBUG);
    /* Sticky entry bits distinguish WFI entry from reaching its hook. See #68. */
    PWRCTRL->SYSPWRSTATUS = PWRCTRL_SYSPWRSTATUS_CORESLEEP_Msk |
                           PWRCTRL_SYSPWRSTATUS_COREDEEPSLEEP_Msk |
                           PWRCTRL_SYSPWRSTATUS_SYSDEEPSLEEP_Msk;
    sleep_armed = true;
    __DSB();
    am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_NORMAL);
    g_sleep_wake_status = PWRCTRL->SYSPWRSTATUS;
    fail("UNEXPECTED_WAKE", 0);
}
