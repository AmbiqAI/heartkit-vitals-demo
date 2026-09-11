// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

static inline float ai_unavailable_rate(void)
{
    const uint32_t bits = UINT32_C(0x7fc00000);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* Fast-math builds cannot use floating-point NaN tests for wire sentinels. */
static inline bool ai_rate_available(float rate)
{
    uint32_t bits;
    memcpy(&bits, &rate, sizeof(bits));
    return bits > 0 && bits < UINT32_C(0x7f800000);
}

static inline float ai_display_rate(float rate, bool enabled)
{
    return enabled && ai_rate_available(rate) ? rate : ai_unavailable_rate();
}

static inline float ai_average_rate(float denoise, float segment, float arrhythmia)
{
    const float rates[] = {denoise, segment, arrhythmia};
    float sum = 0.0f;
    unsigned count = 0;
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t bits;
        memcpy(&bits, &rates[i], sizeof(bits));
        const bool available = bits > 0 && bits < UINT32_C(0x7f800000);
        bits = available ? bits : 0;
        float value;
        memcpy(&value, &bits, sizeof(value));
        sum += value;
        count += available;
    }
    return count ? sum / (float)count : ai_unavailable_rate();
}

/* Clamp sub-microsecond measurements to the timer's representable resolution. */
static inline float ips_from_delta_us(uint32_t deltaUs)
{
    return 1.0e6f / (float)(deltaUs ? deltaUs : 1u);
}

/* The last stage duration approximates each run in the reporting window.
 * Callers must publish duration before advancing the run counter. */
static inline float stage_duty_frac(uint32_t runsDelta, float ips, float windowSec)
{
    if (runsDelta == 0u || ips <= 0.0f || windowSec <= 0.0f) {
        return 0.0f;
    }
    return (1.0f / ips) * ((float)runsDelta / windowSec);
}
