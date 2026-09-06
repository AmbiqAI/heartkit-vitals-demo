// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file tflm_ref.h
 * @brief TFLM reference path for the parity runner.
 *
 * Same runtime, resolver, flatbuffers, arena sizes and placement the firmware
 * uses, driven from the parity runner's bare-metal main(). See #37.
 */
#ifndef __HKV_PARITY_TFLM_REF_H
#define __HKV_PARITY_TFLM_REF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the shared op resolver. Call once, before either model init.
 * @return 0 on success.
 */
int32_t hkv_tflm_ref_init(void);

/**
 * @brief Allocate the segmentation interpreter over its AM_SHARED_RW arena.
 * @return 0 on success, non-zero on schema/allocation failure.
 */
int32_t hkv_tflm_ref_seg_init(void);

int32_t hkv_tflm_ref_arr_init(void);

/**
 * @brief Run one segmentation case.
 *
 * @param in       int8 input, copied verbatim into the input tensor (already
 *                 quantized by the golden generator, as for the AOT path).
 * @param inLen    elements available in @p in.
 * @param out      receives the raw int8 output tensor.
 * @param outLen   capacity of @p out in elements.
 * @param cycles   DWT cycles spanning Invoke() only.
 * @param scale    output tensor dequantization scale.
 * @param zeroPoint output tensor zero point.
 * @return 0 on success, the TfLiteStatus otherwise.
 */
int32_t hkv_tflm_ref_seg_run(const int8_t *in, int inLen, int8_t *out, int outLen, uint32_t *cycles, float *scale,
                             int32_t *zeroPoint);

/**
 * @brief Run one arrhythmia case. Output is dequantized if the model is quantized.
 */
int32_t hkv_tflm_ref_arr_run(const float *in, int inLen, float *out, int outLen, uint32_t *cycles);

#ifdef __cplusplus
}
#endif

#endif // __HKV_PARITY_TFLM_REF_H
