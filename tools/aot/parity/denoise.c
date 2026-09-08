// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "am_mcu_apollo.h"
#include "nsx_core.h"
#include "hkv_denoise_model.h"
#include "golden_den_cases.h"
#include "denoise.h"
#if defined(HKV_PARITY_TFLM)
#include "tflm_ref.h"
#endif

#if defined(HKV_PARITY_TFLM)
static hkv_denoise_model_context_t ctx;
static float reference[GOLDEN_DEN_OUTPUT_LEN];
static int refInit = -1;
static struct {
    int rc, pass, finite;
    float maxAbs;
    uint32_t cycles, refCycles;
} results[2][GOLDEN_DEN_NUM_CASES];

/* Integer inspection survives the firmware's -ffast-math; see #37. */
static int finite_f32(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT32_C(0x7f800000)) != UINT32_C(0x7f800000);
}

void hkv_den_parity_init(void) {
#if defined(HKV_PARITY_TFLM)
    refInit = hkv_tflm_ref_den_init();
#endif
}

void hkv_den_parity_run(int mode) {
    int init = hkv_denoise_model_init(&ctx);
    for (int c = 0; c < GOLDEN_DEN_NUM_CASES; c++) {
        results[mode][c].rc = init ? init : refInit;
        if (init || refInit) { continue; }
        if (ctx.inputs[0].size != sizeof(golden_den_inputs[c]) ||
            ctx.outputs[0].size != sizeof(reference)) {
            results[mode][c].rc = -2;
            continue;
        }
        memcpy(ctx.inputs[0].data, golden_den_inputs[c], sizeof(golden_den_inputs[c]));
        uint32_t start = DWT->CYCCNT;
        int rc = hkv_denoise_model_run(&ctx);
        results[mode][c].cycles = DWT->CYCCNT - start;
#if defined(HKV_PARITY_TFLM)
        int refRc = hkv_tflm_ref_den_run(golden_den_inputs[c], GOLDEN_DEN_INPUT_LEN,
                                        reference, GOLDEN_DEN_OUTPUT_LEN, &results[mode][c].refCycles);
        if (!rc) { rc = refRc; }
#endif
        results[mode][c].rc = rc;
        results[mode][c].pass = rc == 0;
        results[mode][c].finite = 1;
        const float *actual = (const float *)ctx.outputs[0].data;
        for (int i = 0; i < GOLDEN_DEN_OUTPUT_LEN; i++) {
            if (!finite_f32(actual[i]) || !finite_f32(reference[i])) {
                results[mode][c].finite = results[mode][c].pass = 0;
                continue;
            }
            float delta = fabsf(actual[i] - reference[i]);
            if (delta > results[mode][c].maxAbs) { results[mode][c].maxAbs = delta; }
            if (delta > 1e-5f + 1e-5f * fabsf(reference[i])) { results[mode][c].pass = 0; }
        }
    }
}

void hkv_den_parity_report(void) {
    for (int m = 0; m < 2; m++) {
        int passed = 0;
        for (int c = 0; c < GOLDEN_DEN_NUM_CASES; c++) {
            passed += results[m][c].pass;
            float scaled = results[m][c].maxAbs * 1e9f;
            unsigned error = scaled < 4294967040.0f ? (unsigned)scaled : UINT32_MAX;
            nsx_printf("HKV|parity|den mode=%s ref=tflm case=%d rc=%d finite=%d max_abs_nano=%u cycles=%u ref_cycles=%u pass=%d\r\n",
                       m ? "hp" : "lp", c, results[m][c].rc, results[m][c].finite, error,
                       (unsigned)results[m][c].cycles, (unsigned)results[m][c].refCycles, results[m][c].pass);
        }
        nsx_printf("HKV|parity|den_summary mode=%s ref=tflm pass=%d/%d ref_init=%d\r\n",
                   m ? "hp" : "lp", passed, GOLDEN_DEN_NUM_CASES, refInit);
    }
}
#else
void hkv_den_parity_init(void) {}
void hkv_den_parity_run(int mode) { (void)mode; }
void hkv_den_parity_report(void) {
    nsx_printf("HKV|parity|den_skipped reason=no_tflm_reference\r\n");
}
#endif
