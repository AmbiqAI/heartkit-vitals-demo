// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file ecg_arrhythmia.h
 * @author Adam Page (adam.page@ambiq.com)
 * @brief heliaAOT ECG Arrhythmia model
 * @version 1.0
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2024
 *
 */

#ifndef __HK_ECG_ARRHYTHMIA_H
#define __HK_ECG_ARRHYTHMIA_H

#include <stddef.h>
#include <stdint.h>
#include "arm_math.h"

/**
 * @brief Initialize ECG arrhythmia model
 *
 * @return uint32_t
 */
uint32_t
ecg_arrhythmia_init();

/**
 * @brief Scratch arena bytes the arrhythmia model occupies.
 *
 * Exact-fit like the segmentation arena; see ecg_segmentation.h.
 */
size_t
ecg_arrhythmia_arena_used();

size_t
ecg_arrhythmia_arena_size();

/**
 * @brief Run ECG arrhythmia model
 *
 * @param ecgIn ECG input window, ECG_ARR_WINDOW_LEN elements
 * @param threshold Minimum winning class score for a conclusive label
 * @param label Out: class label, ECG_ARR_INCONCLUSIVE when the run fails or scores below threshold
 * @return uint32_t 0 on success, the model run status otherwise
 */
uint32_t
ecg_arrhythmia_inference(float32_t *ecgIn, float32_t threshold, uint32_t *label);

#endif // __HK_ECG_ARRHYTHMIA_H
