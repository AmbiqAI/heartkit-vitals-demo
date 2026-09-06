// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file ecg_arrhythmia.cc
 * @author Adam Page (adam.page@ambiq.com)
 * @brief heliaAOT ECG arrhythmia
 * @version 1.0
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2024
 *
 */
#include <arm_math.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
// Modules
#include "pk_ecg.h"
// NSX runtime
#include "nsx_core.h"
// heliaAOT
#include "hkv_arrhythmia_model.h"
// Locals
#include "store.h"
#include "constants.h"
#include "ecg_arrhythmia.h"
#include "ecg_tensor_copy.h"

/* A model narrower than the host window cannot fill it. The generated I/O
 * extents are compile-time constants, so what #36 caught at boot for
 * segmentation is a build error for both models now. A wider one is filled by
 * edge replication in the copy below, same as denoise and segmentation. */
static_assert(hkv_arrhythmia_input_0_size >= ECG_ARR_WINDOW_LEN, "AOT arr input narrower than the host window");
// Labels are the model's classes shifted by one, ECG_ARR_INCONCLUSIVE taking 0.
static_assert(hkv_arrhythmia_output_0_size == ECG_ARR_GSVT, "AOT arr class count does not match the label map");

static hkv_arrhythmia_model_context_t ecgArrModelCtx = {.callback = nullptr};

/* The I/O descriptor carries no dtype, so element width is the descriptor's
 * byte extent over the I/O element count. This model is float in and out;
 * a regeneration to int8 I/O would need the quantize/dequantize pair back. */
static size_t
arr_elem_width(hkv_arrhythmia_tensor_ident_t id, size_t elems) {
    return elems > 0 ? hkv_arrhythmia_tensor_descriptors[id].size / elems : 0;
}

uint32_t
ecg_arrhythmia_init() {
    hkv_arrhythmia_model_context_t *ctx = &ecgArrModelCtx;

    int32_t status = hkv_arrhythmia_model_init(ctx);
    if (status != hkv_arrhythmia_status_ok) {
        nsx_printf("[ARR] Model init failed: %d\n", (int)status);
        return 1;
    }

    if ((arr_elem_width(ctx->inputs[0].id, hkv_arrhythmia_input_0_size) != sizeof(float32_t)) ||
        (arr_elem_width(ctx->outputs[0].id, hkv_arrhythmia_output_0_size) != sizeof(float32_t))) {
        nsx_printf("[ARR] Unexpected tensor element width\n");
        return 1;
    }

    nsx_printf("[ARR] Arena used: %d bytes\n", (int)ecg_arrhythmia_arena_used());
    return 0;
}

size_t
ecg_arrhythmia_arena_used() {
    return hkv_arrhythmia_arena_sram_size;
}

size_t
ecg_arrhythmia_arena_size() {
    return hkv_arrhythmia_arena_sram_size;
}

uint32_t
ecg_arrhythmia_inference(float32_t *ecgIn, float32_t threshold, uint32_t *label) {
    float32_t yVal, yMax = 0;
    uint32_t yMaxIdx = 0;
    hkv_arrhythmia_model_context_t *ctx = &ecgArrModelCtx;

    *label = ECG_ARR_INCONCLUSIVE;

    // Copy input
    hkv_tensor_input_f32((float32_t *)ctx->inputs[0].data, hkv_tensor_len(hkv_arrhythmia_input_0_size), ecgIn,
                         hkv_host_len(ECG_ARR_WINDOW_LEN));

    // Invoke model
    int32_t runStatus = hkv_arrhythmia_model_run(ctx);
    if (runStatus != hkv_arrhythmia_status_ok) {
        /* Status codes share the label space, so returning one here reads
         * downstream as a rhythm class. The label stays inconclusive. */
        return (uint32_t)runStatus;
    }

    // Copy output
    const float32_t *yOut = (const float32_t *)ctx->outputs[0].data;
    for (int i = 0; i < hkv_arrhythmia_output_0_size; i++) { // CLASSES
        yVal = yOut[i];
        if ((i == 0) || (yVal > yMax)) {
            yMax = yVal;
            yMaxIdx = i;
        }
    }
    // We use 0 to represent inconclusive
#if EN_MODEL_VERBOSE_LOGS
    nsx_printf("yMax=%f, yMaxIdx=%d\n", yMax, yMaxIdx);
#endif
    *label = yMax > threshold ? yMaxIdx + 1 : ECG_ARR_INCONCLUSIVE;
    return 0;
}
