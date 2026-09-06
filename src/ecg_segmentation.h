// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file ecg_segmentation.h
 * @author Adam Page (adam.page@ambiq.com)
 * @brief heliaAOT ECG segmentation
 * @version 1.0
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2023
 *
 */

#ifndef __HK_ECG_SEGMENTATION_H
#define __HK_ECG_SEGMENTATION_H

#include <stddef.h>
#include <stdint.h>
#include "arm_math.h"


/**
 * @brief Initialize ECG segmentation model
 *
 * @return uint32_t
 */
uint32_t
ecg_segmentation_init();

/**
 * @brief Scratch arena bytes the segmentation model occupies.
 *
 * The AOT arena is planned at build time and is exact-fit, so used and size
 * return the same number. Both are kept: the boot line prints used, the arena
 * telemetry logs the pair -- see AmbiqAI/heartkit-vitals-demo#75. Model
 * constants are not counted: they are executed in place from MRAM.
 */
size_t
ecg_segmentation_arena_used();

size_t
ecg_segmentation_arena_size();

/**
 * @brief Run ECG segmentation model
 *
 * @param data ECG data
 * @param segMask Segmentation mask
 * @param padLen Padding length
 * @param threshold Threshold
 * @return uint32_t
 */
uint32_t
ecg_segmentation_inference(float32_t *data, uint16_t *segMask, uint32_t padLen, float32_t threshold, float32_t *qos);

uint32_t
ecg_physiokit_segmentation_inference(float32_t *data, uint16_t *segMask, uint32_t padLen, float32_t *qos);

void
ecg_segmentation_extract_fiducials(uint16_t *segMask, float32_t *data);

#endif // __HK_SEGMENTATION_H
