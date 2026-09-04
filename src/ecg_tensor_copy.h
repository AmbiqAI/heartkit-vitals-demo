// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file ecg_tensor_copy.h
 * @brief Host window <-> model tensor copies for ECG denoise and segmentation.
 *
 * These loops live in a header whose only includes are stdint, stddef and
 * constants.h, so tests/test_ecg_tensor_copy.c can drive them on the host
 * under ASan/UBSan. Inside ecg_denoise.cc and ecg_segmentation.cc they are only
 * reachable with a real TFLM interpreter, where a bound taken from the tensor
 * instead of the host array is a silent out-of-bounds write rather than a
 * failing test.
 *
 * The deployed models are wider than the host windows, so every bound here is
 * the overlap of the two and every tensor element past the host window is
 * defined by edge replication. See #36.
 */
#ifndef __HKV_ECG_TENSOR_COPY_H
#define __HKV_ECG_TENSOR_COPY_H

#include <stddef.h>
#include <stdint.h>

#include "constants.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Length of a host window, in elements. Not interchangeable with hkv_tensor_len_t.
 */
typedef struct {
    int n;
} hkv_host_len_t;

/**
 * @brief Length of a model tensor's time axis, in elements. Not interchangeable with hkv_host_len_t.
 */
typedef struct {
    int n;
} hkv_tensor_len_t;

/* LIMIT OF THE GUARD. Both constructors take a bare int, so these tags catch a
 * TRANSPOSED pair (host length passed where a tensor length is wanted) but not
 * a MISLABELLED one (the wrong constant handed to the right constructor). All
 * four production call sites are correct today; a heavier scheme was judged not
 * worth the complexity. See #36. */
static inline hkv_host_len_t
hkv_host_len(int n) {
    hkv_host_len_t v;
    v.n = n;
    return v;
}

static inline hkv_tensor_len_t
hkv_tensor_len(int n) {
    hkv_tensor_len_t v;
    v.n = n;
    return v;
}

/**
 * @brief Elements the host window and the model tensor have in common.
 */
static inline int
hkv_tensor_span(hkv_host_len_t hostLen, hkv_tensor_len_t tensorLen) {
    return tensorLen.n < hostLen.n ? tensorLen.n : hostLen.n;
}

/**
 * @brief Copy a float host window into a float input tensor, edge replicating the tail.
 */
static inline void
hkv_tensor_input_f32(float *tensor, hkv_tensor_len_t tensorLen, const float *host, hkv_host_len_t hostLen) {
    int filled = hkv_tensor_span(hostLen, tensorLen);
    for (int i = 0; i < filled; i++) { tensor[i] = host[i]; }
    if (filled <= 0) { return; }
    for (int i = filled; i < tensorLen.n; i++) { tensor[i] = tensor[filled - 1]; }
}

/**
 * @brief Quantize a float host window into an int8 input tensor, edge replicating the tail.
 */
static inline void
hkv_tensor_input_i8(int8_t *tensor, hkv_tensor_len_t tensorLen, const float *host, hkv_host_len_t hostLen, float scale,
                    int32_t zeroPoint) {
    int filled = hkv_tensor_span(hostLen, tensorLen);
    for (int i = 0; i < filled; i++) { tensor[i] = (int8_t)(host[i] / scale + (float)zeroPoint); }
    if (filled <= 0) { return; }
    for (int i = filled; i < tensorLen.n; i++) { tensor[i] = tensor[filled - 1]; }
}

/**
 * @brief Copy a float output tensor into a float host window, skipping padLen at each end.
 */
static inline void
hkv_tensor_output_f32(float *host, hkv_host_len_t hostLen, const float *tensor, hkv_tensor_len_t tensorLen, int padLen) {
    int end = hkv_tensor_span(hostLen, tensorLen) - padLen;
    for (int i = padLen; i < end; i++) { host[i] = tensor[i]; }
}

/**
 * @brief Dequantize an int8 output tensor into a float host window, skipping padLen at each end.
 */
static inline void
hkv_tensor_output_i8(float *host, hkv_host_len_t hostLen, const int8_t *tensor, hkv_tensor_len_t tensorLen, int padLen,
                     float scale, int32_t zeroPoint) {
    int end = hkv_tensor_span(hostLen, tensorLen) - padLen;
    for (int i = padLen; i < end; i++) { host[i] = ((float)tensor[i] - (float)zeroPoint) * scale; }
}

/**
 * @brief Reduce a [1 x TIME x CLASSES] segmentation output to a per-sample mask.
 *
 * Exactly one of tensorI8 and tensorF32 is used, selected by tensorI8 being non-NULL.
 *
 * @return Mean winning class score over the copied range, 0 if the range is empty.
 */
static inline float
hkv_seg_output_mask(uint16_t *segMask, hkv_host_len_t maskLen, const int8_t *tensorI8, const float *tensorF32,
                    hkv_tensor_len_t tensorLen, int numClasses, int padLen, float threshold, float scale,
                    int32_t zeroPoint) {
    int end = hkv_tensor_span(maskLen, tensorLen) - padLen;
    int count = end - padLen;
    float avgQos = 0.0f;
    for (int i = padLen; i < end; i++) {
        float yMax = 0.0f;
        uint8_t yMaxIdx = 0;
        uint16_t qosMask = 0;
        for (int j = 0; j < numClasses; j++) {
            int yIdx = i * numClasses + j;
            float yVal = (tensorI8 != NULL) ? (((float)tensorI8[yIdx] - (float)zeroPoint) * scale) : tensorF32[yIdx];
            if ((j == 0) || (yVal > yMax)) {
                yMax = yVal;
                yMaxIdx = (uint8_t)j;
            }
        }
        qosMask = yMax > ECG_QOS_GOOD_THRESH ? 3 : yMax > ECG_QOS_FAIR_THRESH ? 2 : yMax > ECG_QOS_POOR_THRESH ? 1 : 0;
        avgQos += yMax;
        segMask[i] = (uint16_t)(yMax >= threshold ? yMaxIdx : 0);
        segMask[i] |= (uint16_t)((qosMask & SIG_MASK_QOS_MASK) << SIG_MASK_QOS_OFFSET);
    }
    return count > 0 ? avgQos / (float)count : 0.0f;
}

#ifdef __cplusplus
}
#endif

#endif // __HKV_ECG_TENSOR_COPY_H
