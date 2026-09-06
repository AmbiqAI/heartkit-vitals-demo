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
// heliaAOT
#include "hkv_segmentation_model.h"
// Locals
#include "constants.h"
#include "store.h"
#include "ecg_segmentation.h"
#include "ecg_tensor_copy.h"

/* A model narrower than the host window cannot fill it. The generated I/O
 * extents are compile-time constants, so what #36 caught at boot is now a
 * build error. The output is flat [TIME * CLASSES], not TFLM's [1, TIME,
 * CLASSES]. */
static_assert(hkv_segmentation_input_0_size >= ECG_SEG_WINDOW_LEN, "AOT seg input narrower than the host window");
static_assert(hkv_segmentation_output_0_size >= ECG_SEG_WINDOW_LEN * ECG_SEG_NUM_CLASS,
              "AOT seg output narrower than the host window");
static_assert(hkv_segmentation_output_0_size % ECG_SEG_NUM_CLASS == 0, "AOT seg output is not a whole number of frames");
static_assert(hkv_segmentation_output_0_size == hkv_segmentation_input_0_size * ECG_SEG_NUM_CLASS,
              "AOT seg output time axis does not match the input");

// Elements, not bytes: only the descriptor table carries byte extents.
#define SEG_TENSOR_TIME_LEN (hkv_segmentation_output_0_size / ECG_SEG_NUM_CLASS)

static hkv_segmentation_model_context_t ecgSegModelCtx = {.callback = nullptr};

/* The I/O descriptor carries no dtype, so element width is the descriptor's
 * byte extent over the I/O element count. int8 here; a regeneration that
 * changes it would silently reinterpret the arena. */
static size_t
seg_elem_width(hkv_segmentation_tensor_ident_t id, size_t elems) {
    return elems > 0 ? hkv_segmentation_tensor_descriptors[id].size / elems : 0;
}

uint32_t
ecg_segmentation_init() {
    hkv_segmentation_model_context_t *ctx = &ecgSegModelCtx;

    int32_t status = hkv_segmentation_model_init(ctx);
    if (status != hkv_segmentation_status_ok) {
        nsx_printf("[SEG] Model init failed: %d\n", (int)status);
        return 1;
    }

    if ((seg_elem_width(ctx->inputs[0].id, hkv_segmentation_input_0_size) != sizeof(int8_t)) ||
        (seg_elem_width(ctx->outputs[0].id, hkv_segmentation_output_0_size) != sizeof(int8_t))) {
        nsx_printf("[SEG] Unexpected tensor element width\n");
        return 1;
    }

    nsx_printf("[SEG] Arena used: %d bytes\n", (int)ecg_segmentation_arena_used());
    return 0;
}

size_t
ecg_segmentation_arena_used() {
    return hkv_segmentation_arena_sram_size;
}

size_t
ecg_segmentation_arena_size() {
    return hkv_segmentation_arena_sram_size;
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
    hkv_segmentation_model_context_t *ctx = &ecgSegModelCtx;

    // Copy input and quantize
    hkv_tensor_input_i8(ctx->inputs[0].data, hkv_tensor_len(hkv_segmentation_input_0_size), data,
                        hkv_host_len(ECG_SEG_WINDOW_LEN), ctx->inputs[0].scale, ctx->inputs[0].zero_point);

    // Invoke model
    int32_t runStatus = hkv_segmentation_model_run(ctx);
    if (runStatus != hkv_segmentation_status_ok) { return (uint32_t)runStatus; }

    // Extract output and segmentation mask ([TIME x CLASSES])
    avgQos = hkv_seg_output_mask(segMask, hkv_host_len(ECG_SEG_WINDOW_LEN), ctx->outputs[0].data, nullptr,
                                 hkv_tensor_len(SEG_TENSOR_TIME_LEN), ECG_SEG_NUM_CLASS, (int)padLen, threshold,
                                 ctx->outputs[0].scale, ctx->outputs[0].zero_point);
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
