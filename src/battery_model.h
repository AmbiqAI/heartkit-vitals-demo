// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#ifndef HKV_BATTERY_MODEL_H
#define HKV_BATTERY_MODEL_H

#include <stdbool.h>
#include "constants.h"

typedef struct {
    float sleep_mw;
    float compute_mw;
    float denoise_mw;
    float segment_mw;
    float arrhythmia_mw;
    float margin;
    float capacity_mwh;
} hkv_battery_profile_t;

typedef struct {
    float inference_fraction;
    float average_mw;
    float days;
} hkv_battery_estimate_t;

/* The projection assumes quiet idle, not the streaming firmware's idle loop.
 * Measurement boundaries and budgeting choices: see AmbiqAI/heartkit-vitals-demo#68. */
static inline hkv_battery_profile_t
hkv_battery_profile(bool hp)
{
    const float inference = hp ? MCU_INFERENCE_POWER_MW_HP : MCU_INFERENCE_POWER_MW_LP;
    hkv_battery_profile_t p = {
        MCU_SLEEP_POWER_MW,
        (float)(hp ? MCU_COMPUTE_POWER_MW_HP : MCU_COMPUTE_POWER_MW_LP),
        inference, inference, inference, SYSTEM_POWER_MARGIN, BATT_POWER_CAP
    };
    if (!hp) {
        p.sleep_mw = 1.268f;
        p.compute_mw = 6.0f;
        p.denoise_mw = 8.968f;
        p.segment_mw = 7.443f;
        p.arrhythmia_mw = 8.638f;
    }
    return p;
}

static inline float
hkv_battery_fraction(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    return value > 1.0f ? 1.0f : value;
}

/**
 * Project runtime from finite wall-time fractions over a common window.
 * Stage fractions include their pipeline overhead. Fractions are clamped to
 * [0,1]; stages exceeding total busy time are scaled proportionally to fit.
 * Profile powers are total rail mW, capacity is mWh, and margin is in (0,1].
 * Average power is the duty-weighted sum divided by margin; runtime in days
 * is capacity / average power / 24. Active power already includes idle draw.
 */
static inline hkv_battery_estimate_t
hkv_battery_estimate(hkv_battery_profile_t p, float busy, float den, float seg, float arr)
{
    busy = hkv_battery_fraction(busy);
    den = hkv_battery_fraction(den);
    seg = hkv_battery_fraction(seg);
    arr = hkv_battery_fraction(arr);
    const float total = den + seg + arr;
    const float scale = total > busy ? busy / total : 1.0f;
    const float inference = total * scale;
    const float model_mw = scale * (den * p.denoise_mw + seg * p.segment_mw + arr * p.arrhythmia_mw);
    const float average = (model_mw + (busy - inference) * p.compute_mw +
                           (1.0f - busy) * p.sleep_mw) / p.margin;
    hkv_battery_estimate_t result = {
        inference, average, average > 0.0f ? p.capacity_mwh / average / 24.0f : 0.0f
    };
    return result;
}

#endif
