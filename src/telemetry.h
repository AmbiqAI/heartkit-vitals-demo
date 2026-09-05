// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file telemetry.h
 * @brief Demo-telemetry separation: CPU attribution and duty-cycle projection.
 *
 * Three CPU figures are reported, never one blended number (see #8):
 *
 *   util        raw 100 - idle, what the silicon actually does on this build;
 *   cpu_nodemo  measured minus the TileIO transmit task, i.e. capture and
 *               inference as this build runs them;
 *   cpu_proj    deployment projection -- capture at duty 1.0 plus each
 *               inference stage scaled by its own duty factor.
 *
 * cpu_nodemo subtracts the transmit TASK only. Producer-side pack and CRC work
 * runs inside the pipeline tasks and stays in the figure, so cpu_nodemo is an
 * upper bound on the telemetry-free cost; the HKV_TELEMETRY_ENABLE=OFF image is
 * what measures the remainder.
 *
 * Duty factors are stride/window, derived from the constants that define the
 * pipeline, so retuning a window retunes the projection. Only includes stdbool
 * and constants.h, so tests/test_telemetry.c can drive it on the host.
 */
#ifndef __HKV_TELEMETRY_H
#define __HKV_TELEMETRY_H

#include "constants.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define HKV_TELEMETRY_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define HKV_TELEMETRY_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define HKV_DUTY_FRAC(stride, window) ((float)(stride) / (float)(window))

/* A stage consumes `window` samples and advances by `stride`, so it repeats
 * every stride samples and the window/stride ratio is redundant work that a
 * deployment without overlapped windows would not do. */
#define HKV_DUTY_ECG_DEN HKV_DUTY_FRAC(ECG_DEN_VALID_LEN, ECG_DEN_WINDOW_LEN)
#define HKV_DUTY_ECG_SEG HKV_DUTY_FRAC(ECG_SEG_VALID_LEN, ECG_SEG_WINDOW_LEN)
/* Arrhythmia runs inside the metrics branch and advances on the metrics
 * stride, not on a stride of its own. */
#define HKV_DUTY_ECG_ARR HKV_DUTY_FRAC(ECG_MET_VALID_LEN, ECG_ARR_WINDOW_LEN)
#define HKV_DUTY_ECG_MET HKV_DUTY_FRAC(ECG_MET_VALID_LEN, ECG_MET_WINDOW_LEN)
/* Sensor capture cannot be duty-cycled: every sample is needed. */
#define HKV_DUTY_CAPTURE (1.0f)

/* A stride outside (0, window] means the derivation no longer describes the
 * pipeline -- a zero or negative factor erases a stage from the projection and
 * a factor above 1 invents work. Fail the build rather than report either. */
HKV_TELEMETRY_ASSERT(ECG_DEN_VALID_LEN > 0 && ECG_DEN_VALID_LEN <= ECG_DEN_WINDOW_LEN,
                     "ECG denoise duty factor must be in (0, 1]");
HKV_TELEMETRY_ASSERT(ECG_SEG_VALID_LEN > 0 && ECG_SEG_VALID_LEN <= ECG_SEG_WINDOW_LEN,
                     "ECG segmentation duty factor must be in (0, 1]");
HKV_TELEMETRY_ASSERT(ECG_MET_VALID_LEN > 0 && ECG_MET_VALID_LEN <= ECG_ARR_WINDOW_LEN,
                     "ECG arrhythmia duty factor must be in (0, 1]");
HKV_TELEMETRY_ASSERT(ECG_MET_VALID_LEN > 0 && ECG_MET_VALID_LEN <= ECG_MET_WINDOW_LEN,
                     "ECG metrics duty factor must be in (0, 1]");

/** @brief Coarse CPU attribution, each field a percentage of wall time. */
typedef struct {
    float capture;
    float inference;
    float transport;
    float other;
    float idle;
} hkv_cpu_split_t;

static inline float
hkv_clamp_pct(float pct)
{
    if (pct < 0.0f) {
        return 0.0f;
    }
    return (pct > 100.0f) ? 100.0f : pct;
}

/**
 * @brief Measured busy time with the transmit task removed.
 *
 * @param measPct       Measured 100 - idle, percent of wall time.
 * @param transportPct  Transmit task run time, percent of wall time.
 * @return Percent of wall time, clamped to [0, measPct].
 */
static inline float
hkv_cpu_nodemo_pct(float measPct, float transportPct)
{
    float meas = hkv_clamp_pct(measPct);
    float transport = hkv_clamp_pct(transportPct);
    return (transport >= meas) ? 0.0f : (meas - transport);
}

/**
 * @brief Inference cost the same stages would carry without window overlap.
 *
 * Each stage is scaled by its own duty factor, so this needs per-stage input
 * rather than a single inference total. The metrics branch times arrhythmia and
 * the HR/HRV pass together, so `arrPct` carries both and is billed at the
 * arrhythmia factor, the larger of the two.
 *
 * @param denPct  Denoise run time, percent of wall time.
 * @param segPct  Segmentation run time, percent of wall time.
 * @param arrPct  Arrhythmia/metrics branch run time, percent of wall time.
 * @return Percent of wall time.
 */
static inline float
hkv_duty_inference_pct(float denPct, float segPct, float arrPct)
{
    return hkv_clamp_pct(denPct) * HKV_DUTY_ECG_DEN + hkv_clamp_pct(segPct) * HKV_DUTY_ECG_SEG +
           hkv_clamp_pct(arrPct) * HKV_DUTY_ECG_ARR;
}

/**
 * @brief Deployment projection: capture at full duty plus duty-scaled inference.
 *
 * Everything the split calls `other` -- RTOS overhead, DSP, ring copies, the
 * reporter -- is deliberately absent, so cpu_proj is a floor and the gap to
 * cpu_nodemo is visible on the same line rather than folded away.
 *
 * @param capturePct        Capture run time, percent of wall time.
 * @param dutyInferencePct  Output of hkv_duty_inference_pct().
 * @return Percent of wall time, clamped to [0, 100].
 */
static inline float
hkv_cpu_proj_pct(float capturePct, float dutyInferencePct)
{
    return hkv_clamp_pct(hkv_clamp_pct(capturePct) * HKV_DUTY_CAPTURE + dutyInferencePct);
}

/**
 * @brief Coarse per-component split of one measurement window.
 *
 * The components are measured independently (FreeRTOS run-time counters for
 * capture and transport, DWT stage durations for inference), so they are fitted
 * to `measPct` rather than assumed to add up to it: inference is capped by what
 * is left after capture and transport, and `other` absorbs the remainder.
 *
 * @param measPct       Measured 100 - idle, percent of wall time.
 * @param capturePct    Sensor capture task run time, percent of wall time.
 * @param inferencePct  Summed stage run time, percent of wall time.
 * @param transportPct  Transmit task run time, percent of wall time.
 * @return Split whose fields sum to 100.
 */
static inline hkv_cpu_split_t
hkv_cpu_split(float measPct, float capturePct, float inferencePct, float transportPct)
{
    hkv_cpu_split_t split;
    float meas = hkv_clamp_pct(measPct);
    float headroom;

    split.capture = hkv_clamp_pct(capturePct);
    split.transport = hkv_clamp_pct(transportPct);
    if (split.capture + split.transport > meas) {
        /* Both are direct measurements of the same counters `meas` comes from,
         * so an overshoot is window skew, not a real excess. Scaling keeps the
         * split summing to 100 without picking a winner. */
        float sum = split.capture + split.transport;
        split.capture *= meas / sum;
        split.transport = meas - split.capture;
    }
    headroom = meas - split.capture - split.transport;
    split.inference = hkv_clamp_pct(inferencePct);
    if (split.inference > headroom) {
        split.inference = headroom;
    }
    split.other = headroom - split.inference;
    split.idle = 100.0f - meas;
    return split;
}

#ifdef __cplusplus
}
#endif

#endif // __HKV_TELEMETRY_H
