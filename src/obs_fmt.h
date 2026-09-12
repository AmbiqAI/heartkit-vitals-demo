// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/** @file obs_fmt.h
 * @brief Dependency-free arithmetic for diagnostic serialization.
 */
#ifndef __HKV_OBS_FMT_H
#define __HKV_OBS_FMT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel emitted for a value that is not a number. A parser must treat
 * INT32_MIN as "no reading", not as -21474836.48. It is reachable only from
 * NaN: every finite and infinite input maps elsewhere (see below). */
#define HKV_FX2_INVALID (-2147483647 - 1) /* INT32_MIN, written without limits.h */

/* Largest magnitude that survives the x100 scaling inside int32_t. */
#define HKV_FX2_MAX_INPUT (21474836.0f)

/**
 * @brief Scale a float to hundredths, rounding half away from zero.
 * @param v Value to scale.
 * @return Rounded value clamped to +/-INT32_MAX, or HKV_FX2_INVALID for NaN.
 */
static inline int32_t
hkv_fx2_from_float(float v)
{
    float scaled;

    /* Written as negated comparisons so NaN (for which every comparison is
     * false) falls through to the tie-break below rather than being clamped. */
    if (!(v > -HKV_FX2_MAX_INPUT) || !(v < HKV_FX2_MAX_INPUT)) {
        if (v > 0.0f) {
            return 2147483647; /* +inf or too large */
        }
        if (v < 0.0f) {
            return -2147483647; /* -inf or too small */
        }
        return HKV_FX2_INVALID; /* NaN: not ordered against anything */
    }
    scaled = v * 100.0f;
    return (int32_t)(scaled + ((scaled >= 0.0f) ? 0.5f : -0.5f));
}

/**
 * @brief Interval delta for a free-running counter.
 *
 * MUST stay unsigned. This is what lets a counter be free-running and
 * WRAPPING and never reset -- which is in turn what lets two independent
 * consumers each keep their own snapshot without stealing each other's
 * interval (obs.h). Unsigned subtraction is defined to wrap modulo 2^32, so
 * the answer is correct across the wrap for any interval shorter than 2^32
 * counts, which at this app's rates is decades.
 *
 * The failure mode if this is ever "simplified" to signed arithmetic is
 * specific and ugly: the first report after a wrap emits a delta of roughly
 * 4.29e9 in a `_ps` field, every downstream rate calculation spikes, and
 * nothing in the firmware notices. That is why it is a named function with a
 * test rather than an inline subtraction.
 *
 * @param cur current counter value
 * @param prev value at the previous report
 * @return counts elapsed since prev
 */
static inline uint32_t
hkv_counter_delta(uint32_t cur, uint32_t prev)
{
    return cur - prev;
}

/* Sentinel a gauge's minimum holds when nothing has been observed in the
 * current window. UINT32_MAX specifically, so the ordinary `value < lo` update
 * on the sample path needs no is-this-the-first-observation branch. */
#define HKV_GAUGE_LO_INIT (0xFFFFFFFFu)

/**
 * @brief Value to report for a gauge minimum, mapping the sentinel to 0.
 *
 * The sentinel is an internal encoding, not a measurement, and 0xFFFFFFFF in a
 * `_lo` field would read as a real and alarming occupancy. It is reported as 0
 * instead.
 *
 * THAT SUBSTITUTION IS AMBIGUOUS ON ITS OWN, which is why every gauge also
 * emits an observation count (`<key>_n`): `occ_lo=0` alone cannot distinguish
 * "the pump never ran in this window" from "the pump ran normally against an
 * empty ring", and those are opposite diagnoses. Read `_n` first: `_n=0` means
 * the `_lo`/`_hi` pair carries no information at all.
 *
 * @param lo raw stored minimum
 * @return lo, or 0 if nothing was observed
 */
static inline uint32_t
hkv_gauge_lo_display(uint32_t lo)
{
    return (lo == HKV_GAUGE_LO_INIT) ? 0u : lo;
}

#ifdef __cplusplus
}
#endif

#endif // __HKV_OBS_FMT_H
