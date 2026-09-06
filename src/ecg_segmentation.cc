// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file ecg_segmentation.cc
 * @author Adam Page (adam.page@ambiq.com)
 * @brief ECG segmentation
 * @version 1.0
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2024
 *
 */
#include <arm_math.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
// Modules
#include "pk_ecg.h"
#include "pk_hrv.h"
// NSX runtime
#include "nsx_core.h"
// TFLM
#include "tensorflow/lite/micro/kernels/micro_ops.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_profiler.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/micro/tflite_bridge/micro_error_reporter.h"
#include "tensorflow/lite/schema/schema_generated.h"
// Locals
#include "constants.h"
#include "store.h"
#include "ecg_segmentation_flatbuffer.h"
#include "ecg_segmentation.h"
#include "ecg_tensor_copy.h"


static constexpr int segTensorArenaSize = 1024 * ECG_SEG_MODEL_SIZE_KB;
AM_SHARED_RW alignas(16) static uint8_t segTensorArena[segTensorArenaSize];
tf_model_context_t ecgSegModelCtx = {
    .arenaSize = segTensorArenaSize,
    .arena = segTensorArena,
    .buffer = ecg_segmentation_flatbuffer,
    .model = nullptr,
    .input = nullptr,
    .output = nullptr,
    .interpreter = nullptr,
};

uint32_t
ecg_segmentation_init() {

    size_t bytesUsed;
    TfLiteStatus allocateStatus;
    tf_model_context_t *ctx = &ecgSegModelCtx;

    // Initialize TFLM backend
    tflm_init_model(ctx);

    // Load model
    ctx->model = tflite::GetModel(ctx->buffer);
    if (ctx->model->version() != TFLITE_SCHEMA_VERSION) {
        TF_LITE_REPORT_ERROR(ctx->reporter, "Schema mismatch: given=%d != expected=%d.", ctx->model->version(), TFLITE_SCHEMA_VERSION);
        return 1;
    }

    // Initialize interpreter
    if (ctx->interpreter != nullptr) { ctx->interpreter->Reset(); }
    static tflite::MicroInterpreter static_interpreter(ctx->model, *(ctx->resolver), ctx->arena, ctx->arenaSize, nullptr, ctx->profiler);
    ctx->interpreter = &static_interpreter;

    // Allocate tensors
    allocateStatus = ctx->interpreter->AllocateTensors();
    if (allocateStatus != kTfLiteOk) {
        TF_LITE_REPORT_ERROR(ctx->reporter, "AllocateTensors() failed");
        return 1;
    }

    // Check arena size
    bytesUsed = ctx->interpreter->arena_used_bytes();
    ctx->arenaUsed = bytesUsed;
    nsx_printf("[SEG] Arena used: %d bytes\n", bytesUsed);
    if (bytesUsed > ctx->arenaSize) {
        TF_LITE_REPORT_ERROR(ctx->reporter, "Arena mismatch: given=%d < expected=%d bytes.", ctx->arenaSize, bytesUsed);
        return 1;
    }

    // Store input and output pointers (assume single input/output tensor)
    ctx->input = ctx->interpreter->input(0);
    ctx->output = ctx->interpreter->output(0);

    // A model narrower than the host window cannot fill it. See #36.
    if ((ctx->input->dims->data[1] < ECG_SEG_WINDOW_LEN) || (ctx->output->dims->data[1] < ECG_SEG_WINDOW_LEN)) {
        TF_LITE_REPORT_ERROR(ctx->reporter, "Window mismatch: given=(%d, %d) < expected=%d.", ctx->input->dims->data[1],
                             ctx->output->dims->data[1], ECG_SEG_WINDOW_LEN);
        return 1;
    }
    return 0;
}

uint32_t
ecg_physiokit_segmentation_inference(float32_t *data, uint16_t *segMask, uint32_t padLen, float32_t *qos) {
    uint32_t numPeaks;
    uint16_t qosMask = ECG_QOS_GOOD_THRESH;
    numPeaks = pk_ecg_find_peaks_f32(&ecgPkPeakCtx, data, ECG_SEG_WINDOW_LEN, peaksMetrics, segMask);
    for (size_t i = 0; i < ECG_SEG_WINDOW_LEN; i++) {
        segMask[i] = segMask[i] > 0 ? ECG_SEG_QRS : ECG_SEG_NONE;
        segMask[i] |= ((qosMask & SIG_MASK_QOS_MASK) << SIG_MASK_QOS_OFFSET);
    }
    /* Gated to match ecg_arrhythmia.cc, and not merely for volume.
     *
     * This is the DSP segmentation path (SegmentationModeDsp, selectable over
     * UIO at runtime), and it runs once per ~2 s window on EcgProcessTask. It
     * was emitting 1 + numPeaks raw nsx_printf lines per window in steady
     * state -- not a bring-up or error path. Those calls share am_util_stdio's
     * single file-static g_prfbuf with the serialized HKV report lines (see
     * src/obs.h) and are NOT covered by its lock, so in DSP mode they
     * reproduce exactly the interleaved-buffer corruption that issue #11
     * exists to remove.
     *
     * The mask write below is real work and stays unconditional; only the
     * prints are gated. */
#if EN_MODEL_VERBOSE_LOGS
    nsx_printf("ECG SEG PK numPeaks: %d\n", numPeaks);
#endif
    for (size_t i = 0; i < numPeaks; i++) {
#if EN_MODEL_VERBOSE_LOGS
        nsx_printf("ECG SEG PK %d: %d\n", i, peaksMetrics[i]);
#endif
        segMask[peaksMetrics[i]] |= (ECG_FID_PEAK_QRS << ECG_MASK_FID_PEAK_OFFSET);
    }
    // ecg_segmentation_extract_fiducials(segMask, data);
    *qos = 100;
    return 0;
}

uint32_t
ecg_segmentation_inference(float32_t *data, uint16_t *segMask, uint32_t padLen, float32_t threshold, float32_t *qos) {
    float32_t avgQos = 0;
    tf_model_context_t *ctx = &ecgSegModelCtx;

    // Copy input and quantize
    if (ctx->input->quantization.type == kTfLiteAffineQuantization) {
        hkv_tensor_input_i8(ctx->input->data.int8, hkv_tensor_len(ctx->input->dims->data[1]), data,
                            hkv_host_len(ECG_SEG_WINDOW_LEN), ctx->input->params.scale, ctx->input->params.zero_point);
    } else {
        hkv_tensor_input_f32(ctx->input->data.f, hkv_tensor_len(ctx->input->dims->data[1]), data,
                             hkv_host_len(ECG_SEG_WINDOW_LEN));
    }

    // Invoke model
    TfLiteStatus invokeStatus = ctx->interpreter->Invoke();
    if (invokeStatus != kTfLiteOk) { return invokeStatus; }

    // Extract output and segmentation mask ([BATCH x TIME x CLASSES])
    bool isQuantized = ctx->output->quantization.type == kTfLiteAffineQuantization;
    avgQos = hkv_seg_output_mask(segMask, hkv_host_len(ECG_SEG_WINDOW_LEN),
                                 isQuantized ? ctx->output->data.int8 : nullptr,
                                 isQuantized ? nullptr : ctx->output->data.f,
                                 hkv_tensor_len(ctx->output->dims->data[1]), ctx->output->dims->data[2], (int)padLen,
                                 threshold, ctx->output->params.scale, ctx->output->params.zero_point);
    *qos = 100*avgQos;

    // if (avgQos < ECG_QOS_BAD_AVG_THRESH) {
    //     for (int i = padLen; i < ctx->output->dims->data[1] - (int)padLen; i++) {
    //         segMask[i] = 0;
    //         data[i] = 0;
    //     }
    // }

    ecg_segmentation_extract_fiducials(segMask, data);

    return 0;
}

void
ecg_segmentation_extract_fiducials(uint16_t *segMask, float32_t *data)
{
    // Extract fiducial points
    uint16_t prevSegVal = segMask[0] & SIG_MASK_SEG_MASK;
    uint16_t segVal = 0;
    int startIdx = 0;
    float32_t maxVal = std::abs(data[0]);
    int maxIdx = 0;
    for (size_t i = 1; i < ECG_SEG_WINDOW_LEN; i++)
    {
        // If start of segment, reset max
        segVal = segMask[i] & SIG_MASK_SEG_MASK;
        if ((segVal != 0) && (prevSegVal == 0))
        {
            startIdx = i;
            maxVal = std::abs(data[i]);
            maxIdx = i;
        }
        // If end of segment, mark fiducial
        else if ((segVal == 0) && (prevSegVal != 0))
        {
            if (startIdx >= 0 && (i - startIdx > 2))
            {
                // Fiducial peak value (e.g p-peak) will be same as segmentation value (e.g. p-wave)
                segMask[maxIdx] |= (prevSegVal << ECG_MASK_FID_PEAK_OFFSET);
                // nsx_printf("Segment (%d, %d, %d): Fiducial (%d, %f)\n", segMask[maxIdx], startIdx, i, maxIdx, maxVal);
            }
            startIdx = -1;
        }
        else if (startIdx >= 0)
        {
            if (std::abs(data[i]) > maxVal)
            {
                maxVal = std::abs(data[i]);
                maxIdx = i;
            }
        }
        prevSegVal = segVal;
    }
}
