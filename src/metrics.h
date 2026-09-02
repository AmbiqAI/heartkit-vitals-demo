// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file metrics.h
 * @author Adam Page (adam.page@ambiq.com)
 * @brief Compute heartkit metrics
 * @version 1.0
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2023
 *
 */
#ifndef __HK_METRICS_H
#define __HK_METRICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <arm_math.h>
#include "bio_spo2_a0_typedefs.h"

/**
 * @brief Metrics configuration
 *
 */
typedef struct {
} metrics_config_t;

/**
 * @brief Metrics ECG results
 *
 */
typedef struct {
    float32_t hr;
    float32_t hrv;
    float32_t denoiseCossim;
    float32_t arrhythmiaLabel;
    float32_t denoiseIps;
    float32_t segmentIps;
    float32_t arrhythmiaIps;
    float32_t qos;
    float32_t denoiseuIpspw;
    float32_t segmentuIpspw;
    float32_t arrhythmiaIpspw;
} metrics_ecg_results_t;


/**
 * @brief Metrics PPG results
 *
 */
typedef struct {
    float32_t pr;  // Pulse rate (bpm)
    float32_t spo2;  // Blood oxygen saturation (%)
    float32_t qos; // Quality of signal
} metrics_ppg_results_t;


/**
 * @brief Initialize metrics
 *
 * @param ctx Metrics context
 * @return uint32_t
 */
uint32_t
metrics_init(metrics_config_t *ctx);

uint32_t
metrics_capture_ecg(
    metrics_config_t *ctx,
    float32_t *ecg,
    uint16_t *ecgMask,
    size_t len,
    metrics_ecg_results_t *results
);

uint32_t
metrics_capture_ppg(
    metrics_config_t *ctx,
    float32_t *ppg1,
    float32_t *ppg2,
    size_t len,
    const bio_spo2_a0_configuration_t *p_spo2_cfg,
    metrics_ppg_results_t *results
);

#ifdef __cplusplus
}
#endif

#endif // __HK_METRICS_H
