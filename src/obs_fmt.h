/**
 * @file obs_fmt.h
 * @brief Pure value formatting for the HKV diagnostic line format.
 *
 * Deliberately dependency-free (stdint only): no FreeRTOS, no nsx, no float
 * printf. That is what lets tests/test_obs_format.c exercise it on the host
 * under UBSan, which is the only place the edge cases below are actually
 * checked -- the firmware never sees a NaN on the bench until it does.
 *
 * See obs.h for the line format and the counter/gauge model.
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
 * @brief Scale a float to hundredths as a signed integer.
 *
 * REPLACES the idiom this file exists to kill:
 *
 *     printf("%d.%02d", (int)x, (int)(fabsf(x - (int)x) * 100))
 *
 * which is wrong twice. It LOSES THE SIGN for -1 < x < 0 (the integer part is
 * 0, prints as "0.50" for -0.5), and it pays for an fabsf, a subtraction and
 * two float->int conversions per field. This does one multiply, one add and
 * one conversion, and the sign is carried by the single integer it returns.
 *
 * The emitted key carries an `_x100` suffix so the scale is self-describing on
 * the wire and a consumer never has to guess (see hkv_log_fx2).
 *
 * Rounding is half-away-from-zero, so +0.005 -> 1 and -0.005 -> -1 and the
 * result is symmetric about zero. Truncation would bias every negative value
 * one count toward zero.
 *
 * Out-of-range and non-finite inputs are handled explicitly rather than left
 * to the C cast: converting a NaN or an out-of-range float to int32_t is
 * undefined behaviour, and these values genuinely occur -- ppgMetResults.spo2
 * is a ratio of two measured amplitudes and is NaN until the first valid PPG
 * window lands.
 *
 * @param v value to scale
 * @return v * 100 rounded, clamped to +/-INT32_MAX; HKV_FX2_INVALID for NaN
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

#ifdef __cplusplus
}
#endif

#endif // __HKV_OBS_FMT_H
