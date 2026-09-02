// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file metrics.c
 * @author Adam Page (adam.page@ambiq.com)
 * @brief Compute physiokit metrics
 * @version 1.0
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2023
 *
 */
#include <stdbool.h>
#include <math.h>
#include <arm_math.h>
// Modules
#include "pk_math.h"
#include "pk_ecg.h"
#include "pk_ppg.h"
#include "pk_hrv.h"
#include "pk_filter.h"
// Locals
#include "constants.h"
#include "metrics.h"
#include "store.h"

static float32_t g_ppg_peak_state[4 * PPG_MET_WINDOW_LEN];
static uint32_t g_ppg_peaks[PPG_MET_WINDOW_LEN];
static uint32_t g_ppg_rr_intervals[PPG_MET_WINDOW_LEN];
static uint8_t g_ppg_rr_mask[PPG_MET_WINDOW_LEN];
static uint32_t g_ppg_rr_mask_u32[PPG_MET_WINDOW_LEN];
static float32_t g_ppg1_demean[PPG_MET_WINDOW_LEN];
static float32_t g_ppg2_demean[PPG_MET_WINDOW_LEN];
static ppg_peak_f32_t g_ppg_peak_ctx = {
    .peakWin = 0.111f,
    .beatWin = 0.667f,
    .beatOffset = 0.02f,
    .peakDelayWin = 0.3f,
    .sampleRate = PPG_TARGET_RATE,
    .state = g_ppg_peak_state,
    .peaks = g_ppg_peaks,
};
static const float32_t g_spo2_coefs[3] = {-45.060f, 30.354f, 94.845f};

static bool
spo2_profile_to_hk_coefs(
    const bio_spo2_a0_configuration_t *p_spo2_cfg,
    float32_t *p_a,
    float32_t *p_b,
    float32_t *p_c)
{
    if ((p_spo2_cfg == NULL) || (p_a == NULL) || (p_b == NULL) || (p_c == NULL)) {
        return false;
    }
    // Treat AMS coefficients as signed fixed-point for HK fallback path.
    // This keeps profile-driven tuning possible while retaining a safety fallback.
    *p_a = (float32_t)((int16_t)p_spo2_cfg->a) / 1000.0f;
    *p_b = (float32_t)((int16_t)p_spo2_cfg->b) / 1000.0f;
    *p_c = (float32_t)((int16_t)p_spo2_cfg->c) / 100.0f;
    return true;
}

uint32_t
metrics_init(metrics_config_t *ctx) {
    uint32_t err = 0;
    return err;
}

uint32_t
metrics_capture_ecg(
    metrics_config_t *ctx,
    float32_t *ecg,
    uint16_t *ecgMask,
    size_t len,
    metrics_ecg_results_t *results
) {
    uint32_t err = 0;
    uint16_t peakVal, beatVal;
    // float32_t badPeakPerc = 0;
    size_t numPPeaks = 0, numQrsPeaks = 0, numTPeaks = 0, numBeats = 0, numNoiseBeats = 0;
    float32_t hr = 0;

    // Extract fiducial candidates from mask
    numQrsPeaks = 0;
    for (size_t i = 0; i < len; i++) {
        peakVal = (ecgMask[i] >> ECG_MASK_FID_PEAK_OFFSET) & ECG_MASK_FID_PEAK_MASK;
        if (peakVal == ECG_FID_PEAK_QRS) {
            peaksMetrics[numQrsPeaks] = i;
            numQrsPeaks++;
        } else if (peakVal == ECG_FID_PEAK_PPEAK) {
            numPPeaks++;
        } else if (peakVal == ECG_FID_PEAK_TPEAK) {
            numTPeaks++;
        }
    }

    // Filter RR intervals and peaks
    pk_ecg_compute_rr_intervals(peaksMetrics, numQrsPeaks, rriMetrics);
    pk_ecg_filter_rr_intervals(rriMetrics, numQrsPeaks, rriMask, ECG_TARGET_RATE, MIN_RR_SEC, MAX_RR_SEC, MIN_RR_DELTA);
    pk_hrv_compute_time_metrics_from_rr_intervals(rriMetrics, numQrsPeaks, rriMask, &ecgHrvMetrics, ECG_TARGET_RATE);

    // Annotate mask with beats
    for (size_t i = 0; i < numQrsPeaks; i++) {
        if (rriMask[i] == 1) {
            beatVal = ECG_FID_BEAT_NOISE;
            numNoiseBeats += 1;
        } else {
            hr += 60.0f / (rriMetrics[i] / (float32_t)ECG_TARGET_RATE);
            beatVal = ECG_FID_BEAT_NSR;
            numBeats += 1;
        }
        ecgMask[peaksMetrics[i]] |= ((beatVal & ECG_MASK_FID_BEAT_MASK) << ECG_MASK_FID_BEAT_OFFSET);
    }
    hr /= MAX(1, numBeats);

    results->hr = hr;
    results->hrv = ecgHrvMetrics.rmsSD;

    // ns_lp_printf("nPWAVE=%d, nQRS=%d, nTWAVE=%d\n", numPPeaks, numQrsPeaks, numTPeaks);
    // ns_lp_printf("nBEATS=%d, nNOISE=%d, HR=%0.2f, HRV=%0.2f\n\n", numBeats, numNoiseBeats, results->hr, results->hrv);
    return err;
}

uint32_t
metrics_capture_ppg(
    metrics_config_t *ctx,
    float32_t *ppg1,
    float32_t *ppg2,
    size_t len,
    const bio_spo2_a0_configuration_t *p_spo2_cfg,
    metrics_ppg_results_t *results
) {
    uint32_t err = 0;
    float32_t ppg1Mean = 0, ppg2Mean = 0;
    float32_t ppg1Ac = 0, ppg2Ac = 0;
    float32_t pr_bps = 0, spo2 = 0, qos = 0;
    float32_t acdc1 = 0, acdc2 = 0;
    float32_t ppg1Dc = 0, ppg2Dc = 0;
    float32_t spo2_a = g_spo2_coefs[0];
    float32_t spo2_b = g_spo2_coefs[1];
    float32_t spo2_c = g_spo2_coefs[2];
    float32_t spo2_coefs[3] = {0};
    float32_t sigQ = 0, peakQ = 0;
    size_t procLen = len;
    size_t numPeaks = 0;
    size_t numValidPeaks = 0;

    M_UNUSED_PARAM(ctx);

    if ((ppg1 == NULL) || (ppg2 == NULL) || (results == NULL) || (len < 8)) {
        return 1;
    }

    if (procLen > PPG_MET_WINDOW_LEN) {
        procLen = PPG_MET_WINDOW_LEN;
    }
    if (procLen > PPG_MET_BUF_LEN) {
        procLen = PPG_MET_BUF_LEN;
    }

    arm_mean_f32(ppg1, procLen, &ppg1Mean);
    arm_mean_f32(ppg2, procLen, &ppg2Mean);

    for (size_t i = 0; i < procLen; i++) {
        g_ppg1_demean[i] = ppg1[i] - ppg1Mean;
        g_ppg2_demean[i] = ppg2[i] - ppg2Mean;
    }

    numPeaks = pk_ppg_find_peaks_f32(&g_ppg_peak_ctx, g_ppg1_demean, procLen, g_ppg_peaks);
    if (numPeaks > 2) {
        if (numPeaks > PPG_MET_WINDOW_LEN) {
            numPeaks = PPG_MET_WINDOW_LEN;
        }
        pk_ppg_compute_rr_intervals(g_ppg_peaks, numPeaks, g_ppg_rr_intervals);
        pk_ppg_filter_rr_intervals(
            g_ppg_rr_intervals, numPeaks, g_ppg_rr_mask, PPG_TARGET_RATE, MIN_RR_SEC, MAX_RR_SEC, MIN_RR_DELTA);
        for (size_t i = 0; i < numPeaks; i++) {
            g_ppg_rr_mask_u32[i] = g_ppg_rr_mask[i];
        }
        pr_bps = pk_ppg_compute_heart_rate_from_rr_intervals(
            g_ppg_rr_intervals, g_ppg_rr_mask_u32, numPeaks, PPG_TARGET_RATE);
        for (size_t i = 0; i < numPeaks; i++) {
            numValidPeaks += (g_ppg_rr_mask[i] == 0) ? 1 : 0;
        }
    }

    arm_rms_f32(g_ppg1_demean, procLen, &ppg1Ac);
    arm_rms_f32(g_ppg2_demean, procLen, &ppg2Ac);
    if (p_spo2_cfg != NULL) {
        ppg1Dc = ppg1Mean - (float32_t)p_spo2_cfg->dc_comp_red;
        ppg2Dc = ppg2Mean - (float32_t)p_spo2_cfg->dc_comp_ir;
        (void)spo2_profile_to_hk_coefs(p_spo2_cfg, &spo2_a, &spo2_b, &spo2_c);
    } else {
        ppg1Dc = ppg1Mean;
        ppg2Dc = ppg2Mean;
    }
    ppg1Dc = MAX(ppg1Dc, NORM_STD_EPS);
    ppg2Dc = MAX(ppg2Dc, NORM_STD_EPS);
    spo2_coefs[0] = spo2_a;
    spo2_coefs[1] = spo2_b;
    spo2_coefs[2] = spo2_c;
    spo2 = pk_ppg_compute_spo2_from_perfusion_f32(ppg1Dc, ppg1Ac, ppg2Dc, ppg2Ac, spo2_coefs);
    if (!isfinite(spo2) || (spo2 < 50.0f) || (spo2 > 110.0f)) {
        // Safety fallback to the previously validated HK coefficients.
        spo2 = pk_ppg_compute_spo2_from_perfusion_f32(ppg1Mean, ppg1Ac, ppg2Mean, ppg2Ac, (float32_t *)g_spo2_coefs);
    }

    acdc1 = fabsf(ppg1Ac / (ppg1Dc + NORM_STD_EPS));
    acdc2 = fabsf(ppg2Ac / (ppg2Dc + NORM_STD_EPS));

    peakQ = (numPeaks > 0) ? (100.0f * (float32_t)numValidPeaks / (float32_t)numPeaks) : 0.0f;
    sigQ = 100.0f - 400.0f * fabsf(acdc1 - acdc2);
    sigQ = CLIP(sigQ, 0.0f, 100.0f);
    qos = 0.9f * peakQ + 0.1f * sigQ;

    results->pr = 60.0f * pr_bps;
    results->spo2 = CLIP(spo2, 70.0f, 100.0f);
    results->qos = CLIP(qos, 0.0f, 100.0f);
    return err;
}
