/**
 * @file ecg_segmentation.h
 * @author Adam Page (adam.page@ambiq.com)
 * @brief TFLM ECG segmentation
 * @version 1.0
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2023
 *
 */

#ifndef __HK_ECG_SEGMENTATION_H
#define __HK_ECG_SEGMENTATION_H

#include <stdint.h>
#include "arm_math.h"
#include "tflm.h"

/**
 * @brief Initialize ECG segmentation model
 *
 * @return uint32_t
 */
uint32_t
ecg_segmentation_init();

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
