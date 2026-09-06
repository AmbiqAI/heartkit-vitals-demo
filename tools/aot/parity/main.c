// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file main.c
 * @brief On-device golden parity runner for the heliaAOT segmentation and
 *        arrhythmia modules.
 *
 * Separate bare-metal executable, not a mode of the firmware: the generated
 * `_test_case_run()` zeroes DWT->CYCCNT and may claim the PMU, which the
 * firmware's own latency instrumentation depends on. Boot mirrors what the
 * firmware's main() does before the scheduler starts -- core init, ITM/SWO
 * before any perf-mode switch, then nsx_power_configure() with the same
 * nsx_power_config_t the firmware uses (src/store.c nsxPwrCfg) -- so the cycle
 * counts are taken at the operating points the firmware actually runs at.
 *
 * The whole pass runs twice: once at NSX_POWER_PERF_LOW and once at
 * NSX_POWER_PERF_HIGH, switched the way set_speed_mode() does it. Every line
 * carries mode=lp|hp and the summary carries the clk_hz it was measured at.
 *
 * See AmbiqAI/heartkit-vitals-demo#37.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "am_mcu_apollo.h"

#include "nsx_core.h"
#include "nsx_power.h"

#include "hkv_arrhythmia_model.h"
#include "hkv_arrhythmia_test_case.h"
#include "hkv_segmentation_model.h"
#include "hkv_segmentation_test_case.h"

#include "constants.h"
#include "ecg_tensor_copy.h"

#include "golden_arr_cases.h"
#include "golden_seg_cases.h"

#define SEG_TIME_LEN (GOLDEN_SEG_OUTPUT_LEN / ECG_SEG_NUM_CLASS)
#define ARR_NUM_CLASS (GOLDEN_ARR_OUTPUT_LEN)

/* Pass rule agreed on #37: segmentation must be within one output LSB and
 * produce a byte-identical thresholded mask; arrhythmia must agree on argmax
 * and stay within ~2 LSB of the int8 1/256 probability scale it was distilled
 * from. */
#define SEG_MAX_LSB_ALLOWED (1)
#define ARR_MAX_ABS_ALLOWED (0.008f)

static uint16_t maskActual[SEG_TIME_LEN];
static uint16_t maskGolden[SEG_TIME_LEN];

typedef struct {
    int32_t rc;
    int maxLsb;
    float maxAbs;
    int agreeAll;
    int agreeValid;
    int validCount;
    int maskEq;
    uint32_t cycles;
    int pass;
    int label;
} case_result_t;

typedef enum { MODE_LP = 0, MODE_HP = 1, MODE_COUNT = 2 } run_mode_t;

static const char *const modeNames[MODE_COUNT] = {"lp", "hp"};

static case_result_t segRes[MODE_COUNT][GOLDEN_SEG_NUM_CASES];
static case_result_t arrRes[MODE_COUNT][GOLDEN_ARR_NUM_CASES];
static uint32_t modeClockHz[MODE_COUNT];
static uint32_t segInitRc[MODE_COUNT];
static uint32_t arrInitRc[MODE_COUNT];
static int segPassCount[MODE_COUNT];
static int arrPassCount[MODE_COUNT];

/* Same mapping src/timebase.c uses, minus the FreeRTOS half: this runner has no
 * scheduler, so making SystemCoreClock truthful is the whole job. The HAL query
 * is the read side of the mode select nsx_power_* performs; the two frequencies
 * come from the HAL's own macros. Without this, cycles/us conversions and the
 * reported clk_hz keep the boot value after the HP switch. Issue #25. */
static void
sync_core_clock(void)
{
#if defined(AM_PART_APOLLO510B) || defined(AM_PART_APOLLO510)
    am_hal_pwrctrl_mcu_mode_e eMode;

    if (am_hal_pwrctrl_mcu_mode_status(&eMode) != AM_HAL_STATUS_SUCCESS) { return; }
    switch (eMode) {
    case AM_HAL_PWRCTRL_MCU_MODE_LOW_POWER:
        SystemCoreClock = (uint32_t)AM_HAL_CLKGEN_FREQ_MAX_HZ;
        break;
    case AM_HAL_PWRCTRL_MCU_MODE_HIGH_PERFORMANCE:
        SystemCoreClock = (uint32_t)AM_HAL_CLKGEN_FREQ_HP250_HZ;
        break;
    default:
        break;
    }
#else
    SystemCoreClockUpdate();
#endif
}

static hkv_segmentation_model_context_t segCtx = {.callback = NULL};
static hkv_arrhythmia_model_context_t arrCtx = {.callback = NULL};

/* nsx_printf goes through the newlib-nano vfprintf, which drops %f. Every
 * value printed here is a small non-negative magnitude, so fixed point keeps
 * the log parseable without pulling in the float formatter. */
static void
fmt_fixed(char *buf, float value, uint32_t decimals)
{
    uint32_t scale = 1;
    uint32_t whole, frac;
    for (uint32_t i = 0; i < decimals; i++) { scale *= 10; }
    if (value < 0.0f) { value = -value; }
    uint32_t scaled = (uint32_t)(value * (float)scale + 0.5f);
    whole = scaled / scale;
    frac = scaled % scale;
    switch (decimals) {
    case 2: snprintf(buf, 16, "%u.%02u", (unsigned)whole, (unsigned)frac); break;
    case 4: snprintf(buf, 16, "%u.%04u", (unsigned)whole, (unsigned)frac); break;
    default: snprintf(buf, 16, "%u.%06u", (unsigned)whole, (unsigned)frac); break;
    }
}

static void
fmt_pct(char *buf, int num, int den)
{
    fmt_fixed(buf, den > 0 ? 100.0f * (float)num / (float)den : 0.0f, 2);
}

static void
dwt_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static int
argmax_i8(const int8_t *v, int n)
{
    int best = 0;
    for (int j = 1; j < n; j++) {
        if (v[j] > v[best]) { best = j; }
    }
    return best;
}

static int
argmax_f32(const float *v, int n)
{
    int best = 0;
    for (int j = 1; j < n; j++) {
        if (v[j] > v[best]) { best = j; }
    }
    return best;
}

static int
run_seg_case(run_mode_t mode, int caseIdx)
{
    const int8_t *golden = golden_seg_outputs[caseIdx];
    int8_t *in = segCtx.inputs[0].data;
    const int8_t *out = segCtx.outputs[0].data;
    float scale = segCtx.outputs[0].scale;
    int32_t zp = segCtx.outputs[0].zero_point;

    memcpy(in, golden_seg_inputs[caseIdx], GOLDEN_SEG_INPUT_LEN);

    uint32_t t0 = DWT->CYCCNT;
    int32_t rc = hkv_segmentation_model_run(&segCtx);
    uint32_t cycles = DWT->CYCCNT - t0;

    int maxLsb = 0;
    for (int i = 0; i < GOLDEN_SEG_OUTPUT_LEN; i++) {
        int d = (int)out[i] - (int)golden[i];
        if (d < 0) { d = -d; }
        if (d > maxLsb) { maxLsb = d; }
    }

    int agreeAll = 0, agreeValid = 0, validCount = 0;
    for (int i = 0; i < SEG_TIME_LEN; i++) {
        int a = argmax_i8(&out[i * ECG_SEG_NUM_CLASS], ECG_SEG_NUM_CLASS);
        int g = argmax_i8(&golden[i * ECG_SEG_NUM_CLASS], ECG_SEG_NUM_CLASS);
        int inValid = (i >= ECG_SEG_PAD_LEN) && (i < SEG_TIME_LEN - ECG_SEG_PAD_LEN);
        if (a == g) { agreeAll++; }
        if (inValid) {
            validCount++;
            if (a == g) { agreeValid++; }
        }
    }

    /* Mask equality is the property the firmware actually ships: a sub-LSB
     * output difference that straddles ECG_SEG_THRESHOLD still changes what
     * the host sees. */
    memset(maskActual, 0, sizeof(maskActual));
    memset(maskGolden, 0, sizeof(maskGolden));
    hkv_seg_output_mask(maskActual, hkv_host_len(SEG_TIME_LEN), out, NULL, hkv_tensor_len(SEG_TIME_LEN),
                        ECG_SEG_NUM_CLASS, ECG_SEG_PAD_LEN, ECG_SEG_THRESHOLD, scale, zp);
    hkv_seg_output_mask(maskGolden, hkv_host_len(SEG_TIME_LEN), golden, NULL, hkv_tensor_len(SEG_TIME_LEN),
                        ECG_SEG_NUM_CLASS, ECG_SEG_PAD_LEN, ECG_SEG_THRESHOLD, scale, zp);
    int maskEq = memcmp(maskActual, maskGolden, sizeof(maskActual)) == 0;

    case_result_t *r = &segRes[mode][caseIdx];
    r->rc = rc;
    r->maxLsb = maxLsb;
    r->maxAbs = (float)maxLsb * scale;
    r->agreeAll = agreeAll;
    r->agreeValid = agreeValid;
    r->validCount = validCount;
    r->maskEq = maskEq;
    r->cycles = cycles;
    r->pass = (rc == hkv_segmentation_status_ok) && (maxLsb <= SEG_MAX_LSB_ALLOWED) && maskEq;
    return r->pass;
}

static int
run_arr_case(run_mode_t mode, int caseIdx)
{
    const float *golden = golden_arr_outputs[caseIdx];
    float *in = (float *)arrCtx.inputs[0].data;
    const float *out = (const float *)arrCtx.outputs[0].data;

    memcpy(in, golden_arr_inputs[caseIdx], GOLDEN_ARR_INPUT_LEN * sizeof(float));

    uint32_t t0 = DWT->CYCCNT;
    int32_t rc = hkv_arrhythmia_model_run(&arrCtx);
    uint32_t cycles = DWT->CYCCNT - t0;

    float maxAbs = 0.0f;
    for (int i = 0; i < ARR_NUM_CLASS; i++) {
        float d = out[i] - golden[i];
        if (d < 0.0f) { d = -d; }
        if (d > maxAbs) { maxAbs = d; }
    }

    int aIdx = argmax_f32(out, ARR_NUM_CLASS);
    int gIdx = argmax_f32(golden, ARR_NUM_CLASS);
    int argmaxEq = (aIdx == gIdx);

    /* ecg_arrhythmia.cc: below threshold the class collapses to
     * ECG_ARR_INCONCLUSIVE, otherwise it is reported as argmax + 1. */
    int aLabel = out[aIdx] > ECG_ARR_THRESHOLD ? aIdx + 1 : ECG_ARR_INCONCLUSIVE;
    int gLabel = golden[gIdx] > ECG_ARR_THRESHOLD ? gIdx + 1 : ECG_ARR_INCONCLUSIVE;
    int labelEq = (aLabel == gLabel);

    case_result_t *r = &arrRes[mode][caseIdx];
    r->rc = rc;
    r->maxLsb = 0;
    r->maxAbs = maxAbs;
    r->agreeAll = argmaxEq;
    r->agreeValid = argmaxEq;
    r->validCount = 1;
    r->maskEq = labelEq;
    r->cycles = cycles;
    r->label = aLabel;
    r->pass = (rc == hkv_arrhythmia_status_ok) && argmaxEq && (maxAbs <= ARR_MAX_ABS_ALLOWED) && labelEq;
    return r->pass;
}

static void
print_mode(run_mode_t mode)
{
    char absBuf[16], allBuf[16], validBuf[16];
    const char *name = modeNames[mode];

    for (int i = 0; i < GOLDEN_SEG_NUM_CASES; i++) {
        const case_result_t *r = &segRes[mode][i];
        fmt_fixed(absBuf, r->maxAbs, 6);
        fmt_pct(allBuf, r->agreeAll, SEG_TIME_LEN);
        fmt_pct(validBuf, r->agreeValid, r->validCount);
        nsx_printf("HKV|parity|seg mode=%s case=%d rc=%d max_lsb=%d max_abs=%s argmax_pct=%s valid_pct=%s "
                   "mask_eq=%d cycles=%u pass=%d\r\n",
                   name, i, (int)r->rc, r->maxLsb, absBuf, allBuf, validBuf, r->maskEq, (unsigned)r->cycles,
                   r->pass);
    }
    /* Same key set as the seg line so one parser handles both: for a float
     * model max_lsb is not defined, argmax_pct/valid_pct are the single-label
     * agreement, and mask_eq is the thresholded label. */
    for (int i = 0; i < GOLDEN_ARR_NUM_CASES; i++) {
        const case_result_t *r = &arrRes[mode][i];
        fmt_fixed(absBuf, r->maxAbs, 6);
        fmt_pct(allBuf, r->agreeAll, r->validCount);
        nsx_printf("HKV|parity|arr mode=%s case=%d rc=%d max_lsb=0 max_abs=%s argmax_pct=%s valid_pct=%s "
                   "mask_eq=%d cycles=%u pass=%d label=%d\r\n",
                   name, i, (int)r->rc, absBuf, allBuf, allBuf, r->maskEq, (unsigned)r->cycles, r->pass,
                   r->label);
    }

    nsx_printf("HKV|parity|summary mode=%s seg_pass=%d/%d arr_pass=%d/%d seg_init_rc=%u arr_init_rc=%u "
               "clk_hz=%u\r\n",
               name, segPassCount[mode], GOLDEN_SEG_NUM_CASES, arrPassCount[mode], GOLDEN_ARR_NUM_CASES,
               (unsigned)segInitRc[mode], (unsigned)arrInitRc[mode], (unsigned)modeClockHz[mode]);
}

static void
print_report(int32_t segSelf, int32_t arrSelf)
{
    nsx_printf("\r\nHKV|parity|boot clk_hz=%u seg_arena=%u arr_arena=%u\r\n", (unsigned)modeClockHz[MODE_LP],
               (unsigned)hkv_segmentation_arena_dtcm_size, (unsigned)hkv_arrhythmia_arena_dtcm_size);
    nsx_printf("HKV|parity|selfcheck model=seg rc=%d\r\n", (int)segSelf);
    nsx_printf("HKV|parity|selfcheck model=arr rc=%d\r\n", (int)arrSelf);

    for (int mode = 0; mode < MODE_COUNT; mode++) { print_mode((run_mode_t)mode); }
    nsx_printf("PARITY_DONE\r\n");
}

/* Numerics are recompared in both modes rather than timed only: the AOT kernels
 * are the same code at either clock, so a difference here would mean the
 * operating point changed the result, which is worth catching for free. */
static void
run_pass(run_mode_t mode)
{
    sync_core_clock();
    dwt_enable();
    modeClockHz[mode] = SystemCoreClock;

    segInitRc[mode] = (uint32_t)hkv_segmentation_model_init(&segCtx);
    if (segInitRc[mode] == hkv_segmentation_status_ok) {
        for (int i = 0; i < GOLDEN_SEG_NUM_CASES; i++) { segPassCount[mode] += run_seg_case(mode, i); }
    }
    arrInitRc[mode] = (uint32_t)hkv_arrhythmia_model_init(&arrCtx);
    if (arrInitRc[mode] == hkv_arrhythmia_status_ok) {
        for (int i = 0; i < GOLDEN_ARR_NUM_CASES; i++) { arrPassCount[mode] += run_arr_case(mode, i); }
    }
}

int
main(void)
{
    nsx_core_config_t coreCfg = {
        .api = &nsx_core_V1_0_0,
    };
    nsx_core_init(&coreCfg);

    /* ITM/SWO BEFORE nsx_power_configure()/the perf-mode switch, for the reason
     * src/main.cc documents: unlocking the DCU briefly powers Crypto, and that
     * handshake hangs on a secure Apollo5 part once the CPU is on a
     * SYSPLL-sourced high-performance clock. */
    nsx_itm_printf_enable();

    /* Byte-for-byte the firmware's nsxPwrCfg (src/store.c), so the measurement
     * sees the same powered domains and the same operating point rather than
     * whatever the boot defaults happen to be. */
    nsx_power_config_t pwrCfg = {
        .api = &nsx_power_V1_0_0,
        .perf_mode = NSX_POWER_PERF_LOW,
        .need_audadc = false,
        .need_ssram = true,
        .need_crypto = true,
        .need_ble = true,
        .need_usb = true,
        .need_iom = true,
        .need_uart = false,
        .small_tcm = false,
        .need_tempco = false,
        .need_itm = true,
        .need_xtal = false,
        .spotmgr_collapse = false,
    };
    nsx_power_configure(&pwrCfg);
    sync_core_clock();
    dwt_enable();

    int32_t segSelf = -1, arrSelf = -1;

    /* Before the case loops: these re-init the module-global context and reset
     * CYCCNT, so they must not land between a model_init and its measured runs. */
    segSelf = hkv_segmentation_test_case_init();
    if (segSelf == 0) { segSelf = hkv_segmentation_test_case_run(); }
    arrSelf = hkv_arrhythmia_test_case_init();
    if (arrSelf == 0) { arrSelf = hkv_arrhythmia_test_case_run(); }

    run_pass(MODE_LP);

    nsx_power_set_performance_mode(NSX_POWER_PERF_HIGH);
    run_pass(MODE_HP);

    /* Measured once, reported forever. `nsx view` can only attach to this
     * secure-reset SoC, so a capture always starts mid-run and a single report
     * at boot is unobservable; a J-Link Commander reset to force one desyncs
     * the trace instead. Repeating lets any capture window see a whole pass. */
    while (1) {
        print_report(segSelf, arrSelf);
        nsx_delay_us(10000000);
    }
}
