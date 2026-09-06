// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file ecg_denoise.cc
 * @author Adam Page (adam.page@ambiq.com)
 * @brief TFLM ECG denoise
 * @version 1.0
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2024
 *
 */
#include "arm_math.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
// NSX runtime
#include "nsx_core.h"
// TFLM
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/tflite_bridge/micro_error_reporter.h"
#include "tensorflow/lite/schema/schema_generated.h"
// Locals
#include "tflm.h"
#include "store.h"
#include "constants.h"
#include "ecg_denoise.h"
#include "ecg_denoise_flatbuffer.h"
#include "ecg_tensor_copy.h"

static constexpr int denTensorArenaSize = 1024 * ECG_DEN_MODEL_SIZE_KB;
AM_SHARED_RW alignas(16) static uint8_t denTensorArena[denTensorArenaSize];
tf_model_context_t ecgDenModelCtx = {
    .arenaSize = denTensorArenaSize,
    .arena = denTensorArena,
    .buffer = ecg_denoise_flatbuffer,
    .model = nullptr,
    .input = nullptr,
    .output = nullptr,
    .interpreter = nullptr,
};

uint32_t
ecg_denoise_init() {

    size_t bytesUsed;
    TfLiteStatus allocateStatus;
    tf_model_context_t *ctx = &ecgDenModelCtx;

    // Initialize TFLM backend
    tflm_init_model(ctx);

    // Load model
    ctx->model = tflite::GetModel(ctx->buffer);
    if (ctx->model->version() != TFLITE_SCHEMA_VERSION) {
        TF_LITE_REPORT_ERROR(ctx->reporter, "Schema mismatch: given=%d != expected=%d.", ctx->model->version(), TFLITE_SCHEMA_VERSION);
        return 1;
    }
    // Initialize interpreter
    if (ctx->interpreter != nullptr) { ctx->interpreter->Reset();}
    static tflite::MicroInterpreter denoise_interpreter(ctx->model, *(ctx->resolver), ctx->arena, ctx->arenaSize, nullptr, ctx->profiler);
    ctx->interpreter = &denoise_interpreter;

    // Allocate tensors
    allocateStatus = ctx->interpreter->AllocateTensors();
    if (allocateStatus != kTfLiteOk) {
        TF_LITE_REPORT_ERROR(ctx->reporter, "AllocateTensors() failed");
        return 1;
    }

    // Check arena size
    bytesUsed = ctx->interpreter->arena_used_bytes();
    ctx->arenaUsed = bytesUsed;
    nsx_printf("[DEN] Arena used: %d bytes\n", bytesUsed);
    if (bytesUsed > ctx->arenaSize) {
        TF_LITE_REPORT_ERROR(ctx->reporter, "Arena mismatch: given=%d < expected=%d bytes.", ctx->arenaSize, bytesUsed);
        return 1;
    }

    // Store input and output pointers (assume single input/output tensor)
    ctx->input = ctx->interpreter->input(0);
    ctx->output = ctx->interpreter->output(0);

    // A model narrower than the host window cannot fill it. See #36.
    if ((ctx->input->dims->data[1] < ECG_DEN_WINDOW_LEN) || (ctx->output->dims->data[1] < ECG_DEN_WINDOW_LEN)) {
        TF_LITE_REPORT_ERROR(ctx->reporter, "Window mismatch: given=(%d, %d) < expected=%d.", ctx->input->dims->data[1],
                             ctx->output->dims->data[1], ECG_DEN_WINDOW_LEN);
        return 1;
    }
    return 0;
}

uint32_t
ecg_denoise_inference(float32_t *ecgIn, float32_t *ecgOut, uint32_t padLen, float32_t threshold) {

    tf_model_context_t *ctx = &ecgDenModelCtx;

    // Copy input and quantize
    if (ctx->input->quantization.type == kTfLiteAffineQuantization) {
        hkv_tensor_input_i8(ctx->input->data.int8, hkv_tensor_len(ctx->input->dims->data[1]), ecgIn,
                            hkv_host_len(ECG_DEN_WINDOW_LEN), ctx->input->params.scale, ctx->input->params.zero_point);
    } else {
        hkv_tensor_input_f32(ctx->input->data.f, hkv_tensor_len(ctx->input->dims->data[1]), ecgIn,
                             hkv_host_len(ECG_DEN_WINDOW_LEN));
    }

    // Invoke model
    TfLiteStatus invokeStatus = ctx->interpreter->Invoke();
    if (invokeStatus != kTfLiteOk) {
        return invokeStatus;
    }

    // Copy output and dequantize
    if (ctx->output->quantization.type == kTfLiteAffineQuantization) {
        hkv_tensor_output_i8(ecgOut, hkv_host_len(ECG_DEN_WINDOW_LEN), ctx->output->data.int8,
                             hkv_tensor_len(ctx->output->dims->data[1]), (int)padLen, ctx->output->params.scale,
                             ctx->output->params.zero_point);
    } else {
        hkv_tensor_output_f32(ecgOut, hkv_host_len(ECG_DEN_WINDOW_LEN), ctx->output->data.f,
                              hkv_tensor_len(ctx->output->dims->data[1]), (int)padLen);
    }
    return 0;
}
