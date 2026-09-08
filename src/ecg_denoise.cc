// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#include "ecg_denoise.h"
#include "constants.h"
#include "ecg_tensor_copy.h"
#include "hkv_denoise_model.h"
#include "nsx_core.h"

static_assert(hkv_denoise_input_0_size >= ECG_DEN_WINDOW_LEN, "AOT denoise input narrower than host window");
static_assert(hkv_denoise_output_0_size == hkv_denoise_input_0_size, "AOT denoise time axes differ");

static hkv_denoise_model_context_t denCtx = {};
static bool denReady = false;

uint32_t
ecg_denoise_init() {
    denReady = false;
    int32_t rc = hkv_denoise_model_init(&denCtx);
    if (rc != hkv_denoise_status_ok) { return (uint32_t)rc; }

    if (denCtx.inputs[0].size != hkv_denoise_input_0_size * sizeof(float) ||
        denCtx.outputs[0].size != hkv_denoise_output_0_size * sizeof(float)) {
        nsx_printf("[DEN] Unexpected tensor element width\n");
        return 1;
    }
    denReady = true;
    nsx_printf("[DEN] Arena used: %u bytes\n", (unsigned)ecg_denoise_arena_used());
    return 0;
}

size_t
ecg_denoise_arena_used() {
    return hkv_denoise_arena_sram_size;
}

size_t
ecg_denoise_arena_size() {
    return hkv_denoise_arena_sram_size;
}

uint32_t
ecg_denoise_inference(float32_t *ecgIn, float32_t *ecgOut, uint32_t padLen, float32_t threshold) {
    (void)threshold;
    if (!denReady || ecgIn == nullptr || ecgOut == nullptr || padLen > ECG_DEN_WINDOW_LEN / 2) { return 1; }
    hkv_tensor_input_f32((float *)denCtx.inputs[0].data, hkv_tensor_len(hkv_denoise_input_0_size),
                         ecgIn, hkv_host_len(ECG_DEN_WINDOW_LEN));
    int32_t rc = hkv_denoise_model_run(&denCtx);
    if (rc != hkv_denoise_status_ok) { return (uint32_t)rc; }
    hkv_tensor_output_f32(ecgOut, hkv_host_len(ECG_DEN_WINDOW_LEN), (const float *)denCtx.outputs[0].data,
                          hkv_tensor_len(hkv_denoise_output_0_size), (int)padLen);
    return 0;
}
