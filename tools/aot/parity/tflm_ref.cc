// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file tflm_ref.cc
 * @brief TFLM reference path for the parity runner.
 *
 * Mirrors ecg_segmentation_init()/ecg_arrhythmia_init() and the invoke halves
 * of their inference functions: same flatbuffers, same resolver (src/tflm.cc,
 * linked as-is), same arena sizes from constants.h, same AM_SHARED_RW
 * placement, same input quantize and output handling. Those two .cc files are
 * not compiled in directly because they also pull store.h and pk_ecg.h, i.e.
 * the FreeRTOS-scheduled app state (ecgPkPeakCtx, peaksMetrics) that this
 * bare-metal image has no way to stand up.
 *
 * See AmbiqAI/heartkit-vitals-demo#37.
 */
#include <cstring>

#include "am_mcu_apollo.h"

#include "nsx_core.h"

#include "constants.h"
#include "ecg_arrhythmia_flatbuffer.h"
#include "ecg_segmentation_flatbuffer.h"
#include "tflm.h"

#include "tflm_ref.h"

/* Byte-identical to the firmware's arenas in src/ecg_segmentation.cc and
 * src/ecg_arrhythmia.cc: size drives the TFLM allocator's packing decisions and
 * the section drives every tensor access latency, so both have to match for the
 * cycle comparison against the AOT path to mean anything. */
static constexpr int segTensorArenaSize = 1024 * ECG_SEG_MODEL_SIZE_KB;
static constexpr int arrTensorArenaSize = 1024 * ECG_ARR_MODEL_SIZE_KB;
AM_SHARED_RW alignas(16) static uint8_t segTensorArena[segTensorArenaSize];
AM_SHARED_RW alignas(16) static uint8_t arrTensorArena[arrTensorArenaSize];

static tf_model_context_t segCtx = {
    .arenaSize = segTensorArenaSize,
    .arena = segTensorArena,
    .buffer = ecg_segmentation_flatbuffer,
    .model = nullptr,
    .input = nullptr,
    .output = nullptr,
    .interpreter = nullptr,
};

static tf_model_context_t arrCtx = {
    .arenaSize = arrTensorArenaSize,
    .arena = arrTensorArena,
    .buffer = ecg_arrhythmia_flatbuffer,
    .model = nullptr,
    .input = nullptr,
    .output = nullptr,
    .interpreter = nullptr,
};

int32_t
hkv_tflm_ref_init(void) {
    return (int32_t)tflm_init();
}

/* Split from the per-model init only below the interpreter: MicroInterpreter
 * has no default constructor, so each model needs its own function-static. */
static int32_t
finish_model(tf_model_context_t *ctx, tflite::MicroInterpreter *interpreter, const char *tag) {
    /* ctx->interpreter is what the run functions test for readiness, so it is
     * published only once the tensors behind it exist. */
    if (interpreter->AllocateTensors() != kTfLiteOk) { return 2; }

    nsx_printf("HKV|parity|tflm_arena model=%s used=%u given=%u\r\n", tag, (unsigned)interpreter->arena_used_bytes(),
               (unsigned)ctx->arenaSize);

    ctx->input = interpreter->input(0);
    ctx->output = interpreter->output(0);
    ctx->interpreter = interpreter;
    return 0;
}

int32_t
hkv_tflm_ref_seg_init(void) {
    tflm_init_model(&segCtx);
    segCtx.model = tflite::GetModel(segCtx.buffer);
    if (segCtx.model->version() != TFLITE_SCHEMA_VERSION) { return 1; }
    static tflite::MicroInterpreter seg_interpreter(segCtx.model, *(segCtx.resolver), segCtx.arena, segCtx.arenaSize,
                                                    nullptr, segCtx.profiler);
    return finish_model(&segCtx, &seg_interpreter, "seg");
}

int32_t
hkv_tflm_ref_arr_init(void) {
    tflm_init_model(&arrCtx);
    arrCtx.model = tflite::GetModel(arrCtx.buffer);
    if (arrCtx.model->version() != TFLITE_SCHEMA_VERSION) { return 1; }
    static tflite::MicroInterpreter arr_interpreter(arrCtx.model, *(arrCtx.resolver), arrCtx.arena, arrCtx.arenaSize,
                                                    nullptr, arrCtx.profiler);
    return finish_model(&arrCtx, &arr_interpreter, "arr");
}

int32_t
hkv_tflm_ref_seg_run(const int8_t *in, int inLen, int8_t *out, int outLen, uint32_t *cycles, float *scale,
                     int32_t *zeroPoint) {
    if (segCtx.interpreter == nullptr) { return -1; }

    /* The golden inputs are already in the model's int8 domain (that is what
     * the AOT path is fed), so this is the copy half of hkv_tensor_input_i8
     * with the quantize step already applied upstream. */
    int n = segCtx.input->bytes < (size_t)inLen ? (int)segCtx.input->bytes : inLen;
    memcpy(segCtx.input->data.int8, in, (size_t)n);
    for (int i = n; i < (int)segCtx.input->bytes; i++) { segCtx.input->data.int8[i] = in[n - 1]; }

    uint32_t t0 = DWT->CYCCNT;
    TfLiteStatus rc = segCtx.interpreter->Invoke();
    *cycles = DWT->CYCCNT - t0;
    if (rc != kTfLiteOk) { return (int32_t)rc; }

    int m = segCtx.output->bytes < (size_t)outLen ? (int)segCtx.output->bytes : outLen;
    memcpy(out, segCtx.output->data.int8, (size_t)m);
    *scale = segCtx.output->params.scale;
    *zeroPoint = segCtx.output->params.zero_point;
    return 0;
}

int32_t
hkv_tflm_ref_arr_run(const float *in, int inLen, float *out, int outLen, uint32_t *cycles) {
    if (arrCtx.interpreter == nullptr) { return -1; }

    bool inQuant = arrCtx.input->quantization.type == kTfLiteAffineQuantization;
    for (int i = 0; i < inLen; i++) {
        if (inQuant) {
            arrCtx.input->data.int8[i] = (int8_t)(in[i] / arrCtx.input->params.scale + arrCtx.input->params.zero_point);
        } else {
            arrCtx.input->data.f[i] = in[i];
        }
    }

    uint32_t t0 = DWT->CYCCNT;
    TfLiteStatus rc = arrCtx.interpreter->Invoke();
    *cycles = DWT->CYCCNT - t0;
    if (rc != kTfLiteOk) { return (int32_t)rc; }

    bool outQuant = arrCtx.output->quantization.type == kTfLiteAffineQuantization;
    for (int i = 0; i < outLen; i++) {
        out[i] = outQuant ? (((float)arrCtx.output->data.int8[i] - (float)arrCtx.output->params.zero_point) *
                             arrCtx.output->params.scale)
                          : arrCtx.output->data.f[i];
    }
    return 0;
}
