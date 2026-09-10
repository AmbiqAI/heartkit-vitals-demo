// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "am_bsp.h"
#include "am_mcu_apollo.h"
#include "nsx_core.h"
#include "nsx_power.h"
#include "hkv_denoise_model.h"
#include "hkv_segmentation_model.h"
#include "hkv_arrhythmia_model.h"
#include "golden_den_cases.h"
#include "golden_seg_cases.h"
#include "golden_arr_cases.h"

/* GPIO0 is J8-1, separate from the sensor and radio; see #68. */
#define GATE_PIN 0
#define TIMER_HZ 375000u
#define WINDOW_TICKS (3u * TIMER_HZ)
#define REPEATS 3
#define PHASES 5

static hkv_denoise_model_context_t den;
static hkv_segmentation_model_context_t seg;
static hkv_arrhythmia_model_context_t arr;
static float den_reference[GOLDEN_DEN_OUTPUT_LEN];
static int8_t seg_reference[GOLDEN_SEG_OUTPUT_LEN];
static float arr_reference[GOLDEN_ARR_OUTPUT_LEN];
static volatile bool timer_done;
static const char *names[PHASES] = {"spin", "denoise", "segment", "arrhythmia", "light_sleep"};
static struct {
    uint32_t calls;
    uint32_t ticks;
    int32_t rc;
    bool output_ok;
} results[REPEATS][PHASES];

void am_stimer_cmpr0_isr(void)
{
    am_hal_stimer_int_clear(AM_HAL_STIMER_INT_COMPAREA);
    timer_done = true;
}

static void gate(bool high)
{
    am_hal_gpio_state_write(GATE_PIN, high ? AM_HAL_GPIO_OUTPUT_SET : AM_HAL_GPIO_OUTPUT_CLEAR);
}

static void fail(uint32_t code)
{
    gate(false);
    nsx_itm_printf_enable();
    while (1) {
        nsx_printf("HKV|power|FAIL code=%u\r\n", (unsigned)code);
        nsx_delay_us(1000000);
    }
}

static void spin_ticks(uint32_t ticks)
{
    uint32_t start = am_hal_stimer_counter_get();
    while ((uint32_t)(am_hal_stimer_counter_get() - start) < ticks) { __NOP(); }
}

static int32_t invoke(int phase)
{
    switch (phase) {
    case 1:
        memcpy(den.inputs[0].data, golden_den_inputs[0], sizeof(golden_den_inputs[0]));
        return hkv_denoise_model_run(&den);
    case 2:
        memcpy(seg.inputs[0].data, golden_seg_inputs[0], sizeof(golden_seg_inputs[0]));
        return hkv_segmentation_model_run(&seg);
    case 3:
        memcpy(arr.inputs[0].data, golden_arr_inputs[0], sizeof(golden_arr_inputs[0]));
        return hkv_arrhythmia_model_run(&arr);
    default: return -1;
    }
}

static bool output_matches(int phase)
{
    switch (phase) {
    case 1: return memcmp(den.outputs[0].data, den_reference, sizeof(den_reference)) == 0;
    case 2: return memcmp(seg.outputs[0].data, seg_reference, sizeof(seg_reference)) == 0;
    case 3: return memcmp(arr.outputs[0].data, arr_reference, sizeof(arr_reference)) == 0;
    default: return true;
    }
}

static bool finite_f32(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

static void initialize_models(void)
{
    if (hkv_denoise_model_init(&den) || hkv_segmentation_model_init(&seg) ||
        hkv_arrhythmia_model_init(&arr)) { fail(2); }
    if (den.inputs[0].size != sizeof(golden_den_inputs[0]) ||
        hkv_segmentation_tensor_descriptors[seg.inputs[0].id].size != sizeof(golden_seg_inputs[0]) ||
        hkv_arrhythmia_tensor_descriptors[arr.inputs[0].id].size != sizeof(golden_arr_inputs[0]) ||
        den.outputs[0].size != sizeof(den_reference) ||
        hkv_segmentation_tensor_descriptors[seg.outputs[0].id].size != sizeof(seg_reference) ||
        hkv_arrhythmia_tensor_descriptors[arr.outputs[0].id].size != sizeof(arr_reference)) { fail(3); }
    memcpy(den.inputs[0].data, golden_den_inputs[0], sizeof(golden_den_inputs[0]));
    memcpy(seg.inputs[0].data, golden_seg_inputs[0], sizeof(golden_seg_inputs[0]));
    memcpy(arr.inputs[0].data, golden_arr_inputs[0], sizeof(golden_arr_inputs[0]));
    for (int phase = 1; phase <= 3; phase++) {
        if (invoke(phase)) { fail(4); }
    }
    memcpy(den_reference, den.outputs[0].data, sizeof(den_reference));
    memcpy(seg_reference, seg.outputs[0].data, sizeof(seg_reference));
    memcpy(arr_reference, arr.outputs[0].data, sizeof(arr_reference));
    for (unsigned i = 0; i < GOLDEN_DEN_OUTPUT_LEN; i++) {
        if (!finite_f32(den_reference[i]) ||
            fabsf(den_reference[i] - golden_den_outputs[0][i]) > 0.001f) { fail(5); }
    }
    for (unsigned i = 0; i < GOLDEN_SEG_OUTPUT_LEN; i++) {
        int delta = (int)seg_reference[i] - (int)golden_seg_outputs[0][i];
        /* Host and MCU kernels differ; use the accepted case-0 bound, see #37. */
        if (delta < -6 || delta > 6) { fail(6); }
    }
    for (unsigned i = 0; i < GOLDEN_ARR_OUTPUT_LEN; i++) {
        if (!finite_f32(arr_reference[i]) ||
            fabsf(arr_reference[i] - golden_arr_outputs[0][i]) > 0.008f) { fail(7); }
    }
}

static void run_window(int repeat, int phase)
{
    timer_done = false;
    am_hal_stimer_int_clear(AM_HAL_STIMER_INT_COMPAREA);
    if (am_hal_stimer_compare_delta_set(0, WINDOW_TICKS)) { fail(8); }
    uint32_t start = am_hal_stimer_counter_get();
    gate(true);
    while (!timer_done) {
        if (phase == 4) {
            /* Mask across the flag check so the wake cannot precede WFI. */
            __disable_irq();
            if (!timer_done) { am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_NORMAL); }
            __enable_irq();
        } else if (phase == 0) {
            __NOP();
        } else {
            int32_t rc = invoke(phase);
            results[repeat][phase].calls++;
            if (rc) {
                results[repeat][phase].rc = rc;
                break;
            }
        }
    }
    gate(false);
    results[repeat][phase].ticks = am_hal_stimer_counter_get() - start;
    results[repeat][phase].output_ok = output_matches(phase);
    spin_ticks(TIMER_HZ);
}

int main(void)
{
    nsx_core_config_t core = {.api = &nsx_core_V1_0_0};
    if (nsx_core_init(&core)) { while (1) {} }
    nsx_itm_printf_enable();
    nsx_power_config_t power = nsx_power_minimal;
    power.perf_mode = NSX_POWER_PERF_LOW;
    power.need_ssram = true;
    power.small_tcm = false;
    power.need_itm = true;
    power.spotmgr_collapse = false;
    if (nsx_power_configure(&power)) { fail(1); }
    SystemCoreClock = AM_HAL_CLKGEN_FREQ_MAX_HZ;
    am_hal_pwrctrl_mcu_mode_e mode;
    if (am_hal_pwrctrl_mcu_mode_status(&mode) || mode != AM_HAL_PWRCTRL_MCU_MODE_LOW_POWER) { fail(9); }
    SysTick->CTRL = 0;
    if (am_hal_gpio_pinconfig(GATE_PIN, am_hal_gpio_pincfg_output)) { fail(10); }
    gate(false);
    /* The Apollo5 power helper does not apply need_ble; see #68. */
    am_hal_gpio_state_write(AM_BSP_GPIO_EM9305_EN, AM_HAL_GPIO_OUTPUT_CLEAR);
    if (am_hal_gpio_pinconfig(AM_BSP_GPIO_EM9305_EN, g_AM_BSP_GPIO_EM9305_EN)) { fail(11); }
    if (am_hal_pwrctrl_periph_disable(AM_BSP_EM9305_IOM)) { fail(12); }
    initialize_models();
    am_hal_stimer_config(AM_HAL_STIMER_HFRC_375KHZ | AM_HAL_STIMER_CFG_COMPARE_A_ENABLE);
    am_hal_stimer_int_clear(0xFFFFFFFFu);
    am_hal_stimer_int_enable(AM_HAL_STIMER_INT_COMPAREA);
    NVIC_ClearPendingIRQ(STIMER_CMPR0_IRQn);
    NVIC_EnableIRQ(STIMER_CMPR0_IRQn);
    __enable_irq();
    for (unsigned remaining = 15; remaining > 0; remaining--) {
        nsx_printf("HKV|power|armed clk_hz=%u starts_in=%u\r\n", (unsigned)SystemCoreClock, remaining);
        spin_ticks(TIMER_HZ);
    }
    am_bsp_itm_printf_disable();
    nsx_power_disable_debug();
    am_hal_pwrctrl_periph_disable(AM_HAL_PWRCTRL_PERIPH_DEBUG);
    spin_ticks(2u * TIMER_HZ);
    /* A short preamble distinguishes test windows from boot GPIO activity. */
    gate(true);
    spin_ticks(TIMER_HZ / 10u);
    gate(false);
    spin_ticks(TIMER_HZ);
    for (int repeat = 0; repeat < REPEATS; repeat++) {
        for (int phase = 0; phase < PHASES; phase++) { run_window(repeat, phase); }
    }
    am_hal_stimer_int_disable(AM_HAL_STIMER_INT_COMPAREA);
    NVIC_DisableIRQ(STIMER_CMPR0_IRQn);
    nsx_itm_printf_enable();
    while (1) {
        for (int repeat = 0; repeat < REPEATS; repeat++) {
            for (int phase = 0; phase < PHASES; phase++) {
                nsx_printf("HKV|power|result repeat=%d phase=%s calls=%u ticks=%u rc=%d output_ok=%d\r\n",
                           repeat, names[phase], (unsigned)results[repeat][phase].calls,
                           (unsigned)results[repeat][phase].ticks, (int)results[repeat][phase].rc,
                           results[repeat][phase].output_ok);
            }
        }
        nsx_printf("POWER_DONE clk_hz=%u\r\n", (unsigned)SystemCoreClock);
        nsx_delay_us(2000000);
    }
}
