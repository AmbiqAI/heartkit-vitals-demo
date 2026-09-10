// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#pragma once
#include <stdint.h>

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
