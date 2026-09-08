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
 * Every case is compared against two references, tagged ref= on each line.
 * ref=tflm is the gate: the same flatbuffer through the TFLM interpreter in
 * this same image, on this same silicon, so a mismatch is attributable to the
 * AOT compiler alone. ref=golden is the host LiteRT capture and is
 * informational -- it also carries LiteRT-vs-TFLM kernel differences, which
 * are not what this runner is asked to gate.
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
#include "denoise.h"

#if defined(HKV_PARITY_TFLM)
#include "tflm_ref.h"
#endif

#define SEG_TIME_LEN (GOLDEN_SEG_OUTPUT_LEN / ECG_SEG_NUM_CLASS)
#define ARR_NUM_CLASS (GOLDEN_ARR_OUTPUT_LEN)

/* Agreed pass rule: segmentation must be within one output LSB and
 * produce a byte-identical thresholded mask; arrhythmia must agree on argmax
 * and stay within ~2 LSB of the int8 1/256 probability scale it was distilled
 * from. */
#define SEG_MAX_LSB_ALLOWED (1)
#define ARR_MAX_ABS_ALLOWED (0.008f)

static uint16_t maskActual[SEG_TIME_LEN];
static uint16_t maskRef[SEG_TIME_LEN];

typedef struct {
    int32_t rc;
    int maxLsb;
    float maxAbs;
    int agreeAll;
    int agreeValid;
    int validCount;
    int maskEq;
    uint32_t cycles;
    uint32_t refCycles;
    int pass;
    int label;
} case_result_t;

typedef enum { MODE_LP = 0, MODE_HP = 1, MODE_COUNT = 2 } run_mode_t;
typedef enum { REF_TFLM = 0, REF_GOLDEN = 1, REF_COUNT = 2 } ref_t;

static const char *const modeNames[MODE_COUNT] = {"lp", "hp"};
static const char *const refNames[REF_COUNT] = {"tflm", "golden"};

/* Skips the tflm slot entirely when the reference path is compiled out, rather
 * than emitting rows the report would then have to know are meaningless. */
#if defined(HKV_PARITY_TFLM)
#define REF_FIRST REF_TFLM
#else
#define REF_FIRST REF_GOLDEN
#endif

static case_result_t segRes[MODE_COUNT][REF_COUNT][GOLDEN_SEG_NUM_CASES];
static case_result_t arrRes[MODE_COUNT][REF_COUNT][GOLDEN_ARR_NUM_CASES];
static uint32_t modeClockHz[MODE_COUNT];
static uint32_t segInitRc[MODE_COUNT];
static uint32_t arrInitRc[MODE_COUNT];
static int segPassCount[MODE_COUNT][REF_COUNT];
static int arrPassCount[MODE_COUNT][REF_COUNT];

/* The AOT output has to be snapshotted before the TFLM invoke rather than
 * compared in place: both runtimes are live in this image and only their arena
 * placement, not their lifetime, is guaranteed disjoint. */
static int8_t aotSegOut[GOLDEN_SEG_OUTPUT_LEN];
static float aotArrOut[ARR_NUM_CLASS];
#if defined(HKV_PARITY_TFLM)
static int8_t tflmSegOut[GOLDEN_SEG_OUTPUT_LEN];
static float tflmArrOut[ARR_NUM_CLASS];
static int32_t tflmSegInitRc = -1;
static int32_t tflmArrInitRc = -1;
#endif

/* Same mapping src/timebase.c uses, minus the FreeRTOS half: this runner has no
 * scheduler, so making SystemCoreClock truthful is the whole job. The HAL query
 * is the read side of the mode select nsx_power_* performs; the two frequencies
 * come from the HAL's own macros. Without this, cycles/us conversions and the
 * reported clk_hz keep the boot value after the HP switch.
 * See AmbiqAI/heartkit-vitals-demo#25. */
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

#if defined(HKV_PARITY_DUMP_OPS)
/* Per-op output dump, opt-in at compile time. Off by default: it emits ~70 KB of
 * hex over SWO per pass and replaces the timed parity pass entirely. Used to
 * localise the segmentation divergence against a LiteRT builtin-kernel dump. */

/* op id -> its output tensor. The enum value is not the LiteRT tensor index
 * (the generator interns extra requant slots), so the LiteRT index is carried
 * separately for the log; it is what the reference npz keys are named after. */
typedef struct {
    hkv_segmentation_tensor_ident_t ident;
    int litert;
} dump_slot_t;

static const dump_slot_t dumpSlots[31] = {
    {hkv_segmentation_tensor_39, 39}, {hkv_segmentation_tensor_40, 40},
    {hkv_segmentation_tensor_41, 41}, {hkv_segmentation_tensor_42, 42},
    {hkv_segmentation_tensor_43, 43}, {hkv_segmentation_tensor_44, 44},
    {hkv_segmentation_tensor_45, 45}, {hkv_segmentation_tensor_46, 46},
    {hkv_segmentation_tensor_47, 47}, {hkv_segmentation_tensor_48, 48},
    {hkv_segmentation_tensor_49, 49}, {hkv_segmentation_tensor_50, 50},
    {hkv_segmentation_tensor_51, 51}, {hkv_segmentation_tensor_52, 52},
    {hkv_segmentation_tensor_53, 53}, {hkv_segmentation_tensor_54, 54},
    {hkv_segmentation_tensor_55, 55}, {hkv_segmentation_tensor_56, 56},
    {hkv_segmentation_tensor_57, 57}, {hkv_segmentation_tensor_58, 58},
    {hkv_segmentation_tensor_59, 59}, {hkv_segmentation_tensor_60, 60},
    {hkv_segmentation_tensor_61, 61}, {hkv_segmentation_tensor_62, 62},
    {hkv_segmentation_tensor_63, 63}, {hkv_segmentation_tensor_64, 64},
    {hkv_segmentation_tensor_65, 65}, {hkv_segmentation_tensor_66, 66},
    {hkv_segmentation_tensor_67, 67}, {hkv_segmentation_tensor_68, 68},
    {hkv_segmentation_tensor_69, 69},
};

#define DUMP_CHUNK 64

/* `len` is the whole tensor; `off` and the hex length locate the chunk. */
static void
dump_op_output(int32_t op)
{
    static const char digits[] = "0123456789abcdef";
    char hex[2 * DUMP_CHUNK + 1];

    if (op < 0 || op >= (int32_t)(sizeof(dumpSlots) / sizeof(dumpSlots[0]))) { return; }
    const dump_slot_t *slot = &dumpSlots[op];
    const uint8_t *data = (const uint8_t *)segCtx.tensor_ptrs[slot->ident];
    int len = (int)hkv_segmentation_tensor_descriptors[slot->ident].size;
    if (data == NULL || len <= 0) { return; }

    for (int off = 0; off < len; off += DUMP_CHUNK) {
        int n = len - off < DUMP_CHUNK ? len - off : DUMP_CHUNK;
        for (int i = 0; i < n; i++) {
            hex[2 * i] = digits[(data[off + i] >> 4) & 0xF];
            hex[2 * i + 1] = digits[data[off + i] & 0xF];
        }
        hex[2 * n] = '\0';
        nsx_printf("HKV|opdump|seg op=%d tensor=%d len=%d off=%d hex=%s\r\n", (int)op, slot->litert, len,
                   off, hex);
    }
}

/* Scratch tensors share arena space, so each output has to be read in its own
 * run_finished before a later op overwrites it. */
static void
dump_callback(int32_t op, hkv_segmentation_operator_state_t state, int32_t status, void *user_data)
{
    (void)status;
    (void)user_data;
    if (state != hkv_segmentation_op_state_run_finished) { return; }
    dump_op_output(op);
}

static void
run_op_dump(void)
{
    segCtx.callback = dump_callback;
    int32_t initRc = hkv_segmentation_model_init(&segCtx);

    while (1) {
        nsx_printf("\r\nHKV|opdump|begin model=seg case=0 init_rc=%d\r\n", (int)initRc);
        int32_t rc = hkv_segmentation_status_ok;
        if (initRc == hkv_segmentation_status_ok) {
            memcpy(segCtx.inputs[0].data, golden_seg_inputs[0], GOLDEN_SEG_INPUT_LEN);
            rc = hkv_segmentation_model_run(&segCtx);
        }
        nsx_printf("HKV|opdump|end model=seg case=0 rc=%d\r\n", (int)rc);
        nsx_printf("PARITY_DONE\r\n");
        nsx_delay_us(10000000);
    }
}
#endif /* HKV_PARITY_DUMP_OPS */

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

/* One comparison body for both references: the pass rule is a property of the
 * model output, not of what it is being held against. */
static void
compare_seg(case_result_t *r, const int8_t *actual, const int8_t *ref, int32_t rc, uint32_t cycles,
            uint32_t refCycles, float scale, int32_t zp, float refScale, int32_t refZp)
{
    int maxLsb = 0;
    for (int i = 0; i < GOLDEN_SEG_OUTPUT_LEN; i++) {
        int d = (int)actual[i] - (int)ref[i];
        if (d < 0) { d = -d; }
        if (d > maxLsb) { maxLsb = d; }
    }

    int agreeAll = 0, agreeValid = 0, validCount = 0;
    for (int i = 0; i < SEG_TIME_LEN; i++) {
        int a = argmax_i8(&actual[i * ECG_SEG_NUM_CLASS], ECG_SEG_NUM_CLASS);
        int g = argmax_i8(&ref[i * ECG_SEG_NUM_CLASS], ECG_SEG_NUM_CLASS);
        int inValid = (i >= ECG_SEG_PAD_LEN) && (i < SEG_TIME_LEN - ECG_SEG_PAD_LEN);
        if (a == g) { agreeAll++; }
        if (inValid) {
            validCount++;
            if (a == g) { agreeValid++; }
        }
    }

    /* Mask equality is the property the firmware actually ships: a sub-LSB
     * output difference that straddles ECG_SEG_THRESHOLD still changes what the
     * host sees. Each side is thresholded with its own quantization, so the
     * comparison stays valid if the reference ever reports a different scale. */
    memset(maskActual, 0, sizeof(maskActual));
    memset(maskRef, 0, sizeof(maskRef));
    hkv_seg_output_mask(maskActual, hkv_host_len(SEG_TIME_LEN), actual, NULL, hkv_tensor_len(SEG_TIME_LEN),
                        ECG_SEG_NUM_CLASS, ECG_SEG_PAD_LEN, ECG_SEG_THRESHOLD, scale, zp);
    hkv_seg_output_mask(maskRef, hkv_host_len(SEG_TIME_LEN), ref, NULL, hkv_tensor_len(SEG_TIME_LEN),
                        ECG_SEG_NUM_CLASS, ECG_SEG_PAD_LEN, ECG_SEG_THRESHOLD, refScale, refZp);
    int maskEq = memcmp(maskActual, maskRef, sizeof(maskActual)) == 0;

    r->rc = rc;
    r->maxLsb = maxLsb;
    r->maxAbs = (float)maxLsb * scale;
    r->agreeAll = agreeAll;
    r->agreeValid = agreeValid;
    r->validCount = validCount;
    r->maskEq = maskEq;
    r->cycles = cycles;
    r->refCycles = refCycles;
    r->pass = (rc == hkv_segmentation_status_ok) && (maxLsb <= SEG_MAX_LSB_ALLOWED) && maskEq;
}

static void
compare_arr(case_result_t *r, const float *actual, const float *ref, int32_t rc, uint32_t cycles, uint32_t refCycles)
{
    float maxAbs = 0.0f;
    for (int i = 0; i < ARR_NUM_CLASS; i++) {
        float d = actual[i] - ref[i];
        if (d < 0.0f) { d = -d; }
        if (d > maxAbs) { maxAbs = d; }
    }

    int aIdx = argmax_f32(actual, ARR_NUM_CLASS);
    int gIdx = argmax_f32(ref, ARR_NUM_CLASS);
    int argmaxEq = (aIdx == gIdx);

    /* ecg_arrhythmia.cc: below threshold the class collapses to
     * ECG_ARR_INCONCLUSIVE, otherwise it is reported as argmax + 1. */
    int aLabel = actual[aIdx] > ECG_ARR_THRESHOLD ? aIdx + 1 : ECG_ARR_INCONCLUSIVE;
    int gLabel = ref[gIdx] > ECG_ARR_THRESHOLD ? gIdx + 1 : ECG_ARR_INCONCLUSIVE;
    int labelEq = (aLabel == gLabel);

    r->rc = rc;
    r->maxLsb = 0;
    r->maxAbs = maxAbs;
    r->agreeAll = argmaxEq;
    r->agreeValid = argmaxEq;
    r->validCount = 1;
    r->maskEq = labelEq;
    r->cycles = cycles;
    r->refCycles = refCycles;
    r->label = aLabel;
    r->pass = (rc == hkv_arrhythmia_status_ok) && argmaxEq && (maxAbs <= ARR_MAX_ABS_ALLOWED) && labelEq;
}

static void
run_seg_case(run_mode_t mode, int caseIdx)
{
    int8_t *in = segCtx.inputs[0].data;
    const int8_t *out = segCtx.outputs[0].data;
    float scale = segCtx.outputs[0].scale;
    int32_t zp = segCtx.outputs[0].zero_point;

    memcpy(in, golden_seg_inputs[caseIdx], GOLDEN_SEG_INPUT_LEN);

    uint32_t t0 = DWT->CYCCNT;
    int32_t rc = hkv_segmentation_model_run(&segCtx);
    uint32_t cycles = DWT->CYCCNT - t0;
    memcpy(aotSegOut, out, GOLDEN_SEG_OUTPUT_LEN);

    /* The golden fixture is the same output tensor captured on host, so it
     * carries the same quantization on both sides. */
    compare_seg(&segRes[mode][REF_GOLDEN][caseIdx], aotSegOut, golden_seg_outputs[caseIdx], rc, cycles, 0, scale, zp,
                scale, zp);

#if defined(HKV_PARITY_TFLM)
    if (tflmSegInitRc == 0) {
        uint32_t refCycles = 0;
        float refScale = scale;
        int32_t refZp = zp;
        memset(tflmSegOut, 0, sizeof(tflmSegOut));
        int32_t refRc = hkv_tflm_ref_seg_run(golden_seg_inputs[caseIdx], GOLDEN_SEG_INPUT_LEN, tflmSegOut,
                                             GOLDEN_SEG_OUTPUT_LEN, &refCycles, &refScale, &refZp);
        /* A failure on either side has to surface as a failing case, so the AOT
         * rc wins and the reference rc only fills in when the AOT side was
         * clean. */
        compare_seg(&segRes[mode][REF_TFLM][caseIdx], aotSegOut, tflmSegOut,
                    rc != hkv_segmentation_status_ok ? rc : refRc, cycles, refCycles, scale, zp, refScale, refZp);
    }
#endif

    for (int ref = REF_FIRST; ref < REF_COUNT; ref++) { segPassCount[mode][ref] += segRes[mode][ref][caseIdx].pass; }
}

static void
run_arr_case(run_mode_t mode, int caseIdx)
{
    float *in = (float *)arrCtx.inputs[0].data;
    const float *out = (const float *)arrCtx.outputs[0].data;

    memcpy(in, golden_arr_inputs[caseIdx], GOLDEN_ARR_INPUT_LEN * sizeof(float));

    uint32_t t0 = DWT->CYCCNT;
    int32_t rc = hkv_arrhythmia_model_run(&arrCtx);
    uint32_t cycles = DWT->CYCCNT - t0;
    memcpy(aotArrOut, out, sizeof(aotArrOut));

    compare_arr(&arrRes[mode][REF_GOLDEN][caseIdx], aotArrOut, golden_arr_outputs[caseIdx], rc, cycles, 0);

#if defined(HKV_PARITY_TFLM)
    if (tflmArrInitRc == 0) {
        uint32_t refCycles = 0;
        memset(tflmArrOut, 0, sizeof(tflmArrOut));
        int32_t refRc = hkv_tflm_ref_arr_run(golden_arr_inputs[caseIdx], GOLDEN_ARR_INPUT_LEN, tflmArrOut,
                                             ARR_NUM_CLASS, &refCycles);
        compare_arr(&arrRes[mode][REF_TFLM][caseIdx], aotArrOut, tflmArrOut,
                    rc != hkv_arrhythmia_status_ok ? rc : refRc, cycles, refCycles);
    }
#endif

    for (int ref = REF_FIRST; ref < REF_COUNT; ref++) { arrPassCount[mode][ref] += arrRes[mode][ref][caseIdx].pass; }
}

static uint32_t
mean_u32(const uint32_t *v, int n)
{
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) { sum += v[i]; }
    return n > 0 ? sum / (uint32_t)n : 0;
}

/* 0 tells the report the reference could not run at all, so it fails on the
 * missing evidence instead of on the zeroed rows it would otherwise see. */
static int
ref_evaluated(ref_t ref)
{
#if defined(HKV_PARITY_TFLM)
    if (ref == REF_TFLM) { return (tflmSegInitRc == 0) && (tflmArrInitRc == 0); }
#else
    (void)ref;
#endif
    return 1;
}

static void
print_mode_ref(run_mode_t mode, ref_t ref)
{
    char absBuf[16], allBuf[16], validBuf[16];
    const char *name = modeNames[mode];
    const char *refName = refNames[ref];

    for (int i = 0; i < GOLDEN_SEG_NUM_CASES; i++) {
        const case_result_t *r = &segRes[mode][ref][i];
        fmt_fixed(absBuf, r->maxAbs, 6);
        fmt_pct(allBuf, r->agreeAll, SEG_TIME_LEN);
        fmt_pct(validBuf, r->agreeValid, r->validCount);
        nsx_printf("HKV|parity|seg mode=%s ref=%s case=%d rc=%d max_lsb=%d max_abs=%s argmax_pct=%s valid_pct=%s "
                   "mask_eq=%d cycles=%u ref_cycles=%u pass=%d\r\n",
                   name, refName, i, (int)r->rc, r->maxLsb, absBuf, allBuf, validBuf, r->maskEq, (unsigned)r->cycles,
                   (unsigned)r->refCycles, r->pass);
    }
    /* Same key set as the seg line so one parser handles both: for a float
     * model max_lsb is not defined, argmax_pct/valid_pct are the single-label
     * agreement, and mask_eq is the thresholded label. */
    for (int i = 0; i < GOLDEN_ARR_NUM_CASES; i++) {
        const case_result_t *r = &arrRes[mode][ref][i];
        fmt_fixed(absBuf, r->maxAbs, 6);
        fmt_pct(allBuf, r->agreeAll, r->validCount);
        nsx_printf("HKV|parity|arr mode=%s ref=%s case=%d rc=%d max_lsb=0 max_abs=%s argmax_pct=%s valid_pct=%s "
                   "mask_eq=%d cycles=%u ref_cycles=%u pass=%d label=%d\r\n",
                   name, refName, i, (int)r->rc, absBuf, allBuf, allBuf, r->maskEq, (unsigned)r->cycles,
                   (unsigned)r->refCycles, r->pass, r->label);
    }

    nsx_printf("HKV|parity|summary mode=%s ref=%s seg_pass=%d/%d arr_pass=%d/%d seg_init_rc=%u arr_init_rc=%u "
               "evaluated=%d clk_hz=%u\r\n",
               name, refName, segPassCount[mode][ref], GOLDEN_SEG_NUM_CASES, arrPassCount[mode][ref],
               GOLDEN_ARR_NUM_CASES, (unsigned)segInitRc[mode], (unsigned)arrInitRc[mode], ref_evaluated(ref),
               (unsigned)modeClockHz[mode]);
}

/* Both runtimes measured in the same image at the same operating point, so the
 * ratio is not carrying a toolchain or power-config difference. tflm=0 means
 * the reference path was compiled out. */
static void
print_cycles(run_mode_t mode)
{
    uint32_t aot[GOLDEN_SEG_NUM_CASES > GOLDEN_ARR_NUM_CASES ? GOLDEN_SEG_NUM_CASES : GOLDEN_ARR_NUM_CASES];
    uint32_t ref[GOLDEN_SEG_NUM_CASES > GOLDEN_ARR_NUM_CASES ? GOLDEN_SEG_NUM_CASES : GOLDEN_ARR_NUM_CASES];

    for (int i = 0; i < GOLDEN_SEG_NUM_CASES; i++) {
        aot[i] = segRes[mode][REF_FIRST][i].cycles;
        ref[i] = segRes[mode][REF_FIRST][i].refCycles;
    }
    nsx_printf("HKV|parity|cycles mode=%s model=seg aot=%u tflm=%u clk_hz=%u\r\n", modeNames[mode],
               (unsigned)mean_u32(aot, GOLDEN_SEG_NUM_CASES), (unsigned)mean_u32(ref, GOLDEN_SEG_NUM_CASES),
               (unsigned)modeClockHz[mode]);

    for (int i = 0; i < GOLDEN_ARR_NUM_CASES; i++) {
        aot[i] = arrRes[mode][REF_FIRST][i].cycles;
        ref[i] = arrRes[mode][REF_FIRST][i].refCycles;
    }
    nsx_printf("HKV|parity|cycles mode=%s model=arr aot=%u tflm=%u clk_hz=%u\r\n", modeNames[mode],
               (unsigned)mean_u32(aot, GOLDEN_ARR_NUM_CASES), (unsigned)mean_u32(ref, GOLDEN_ARR_NUM_CASES),
               (unsigned)modeClockHz[mode]);
}

static void
print_report(int32_t segSelf, int32_t arrSelf)
{
    /* Renamed with the arenas' move out of DTCM into .shared, so the boot line
     * names the section the measurement was actually taken against. */
    nsx_printf("\r\nHKV|parity|boot clk_hz=%u seg_arena_sram=%u arr_arena_sram=%u\r\n",
               (unsigned)modeClockHz[MODE_LP], (unsigned)hkv_segmentation_arena_sram_size,
               (unsigned)hkv_arrhythmia_arena_sram_size);
    nsx_printf("HKV|parity|selfcheck model=seg rc=%d\r\n", (int)segSelf);
    nsx_printf("HKV|parity|selfcheck model=arr rc=%d\r\n", (int)arrSelf);

#if defined(HKV_PARITY_TFLM)
    nsx_printf("HKV|parity|selfcheck model=tflm_seg rc=%d\r\n", (int)tflmSegInitRc);
    nsx_printf("HKV|parity|selfcheck model=tflm_arr rc=%d\r\n", (int)tflmArrInitRc);
#endif

    for (int mode = 0; mode < MODE_COUNT; mode++) {
        for (int ref = REF_FIRST; ref < REF_COUNT; ref++) { print_mode_ref((run_mode_t)mode, (ref_t)ref); }
        print_cycles((run_mode_t)mode);
    }
    hkv_den_parity_report();
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
    hkv_den_parity_run(mode);

    segInitRc[mode] = (uint32_t)hkv_segmentation_model_init(&segCtx);
    if (segInitRc[mode] == hkv_segmentation_status_ok) {
        for (int i = 0; i < GOLDEN_SEG_NUM_CASES; i++) { run_seg_case(mode, i); }
    }
    arrInitRc[mode] = (uint32_t)hkv_arrhythmia_model_init(&arrCtx);
    if (arrInitRc[mode] == hkv_arrhythmia_status_ok) {
        for (int i = 0; i < GOLDEN_ARR_NUM_CASES; i++) { run_arr_case(mode, i); }
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

#if defined(HKV_PARITY_DUMP_OPS)
    run_op_dump();
#else
    int32_t segSelf = -1, arrSelf = -1;

    /* Before the case loops: these re-init the module-global context and reset
     * CYCCNT, so they must not land between a model_init and its measured runs. */
#if defined(HKV_PARITY_TFLM)
    /* Before the AOT self-checks so an AllocateTensors() failure is reported
     * even if a self-check hangs, and once for both modes: the interpreter and
     * its arena are mode independent. A model whose init failed is not compared
     * at all; the summary marks the reference unevaluated so the report fails
     * rather than reading eight silent mismatches as an AOT defect. */
    int32_t tflmRc = hkv_tflm_ref_init();
    tflmSegInitRc = tflmRc == 0 ? hkv_tflm_ref_seg_init() : tflmRc;
    tflmArrInitRc = tflmRc == 0 ? hkv_tflm_ref_arr_init() : tflmRc;
    if (tflmRc == 0) { hkv_den_parity_init(); }
#endif

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
#endif
}
