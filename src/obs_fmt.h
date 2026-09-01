/**
 * @file obs_fmt.h
 * @brief Pure value arithmetic for the HKV diagnostic line format.
 *
 * Everything the line format computes that does NOT need FreeRTOS, nsx, or a
 * lock lives here rather than inside obs.c, for one reason: this header is
 * dependency-free (stdint only), so tests/test_obs_format.c can exercise it on
 * the host under ASan/UBSan. Anything left in obs.c is only reachable on
 * hardware, where a wrong answer shows up as a plausible number in a capture
 * rather than as a failing test.
 *
 * That is not a hypothetical distinction. All three functions below encode a
 * claim that the rest of the design leans on -- the sign of a negative metric,
 * the correctness of a delta across a counter wrap, and the difference between
 * "observed zero" and "observed nothing" -- and each one is a single line that
 * a refactor could plausibly "simplify" into being wrong.
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
