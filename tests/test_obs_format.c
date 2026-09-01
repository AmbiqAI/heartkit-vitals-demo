/**
 * @file test_obs_format.c
 * @brief Host tests for hkv_fx2_from_float (src/obs_fmt.h).
 *
 * This exists because the idiom it replaces was WRONG on hardware and nobody
 * noticed: `printf("%d.%02d", (int)x, (int)(fabsf(x - (int)x) * 100))` prints
 * -0.5 as "0.50", so every negative sub-unit metric in every historical SWO
 * capture read as positive. A sign error that only shows up for a value nobody
 * expected to be negative is exactly the kind that survives review, so it gets
 * a test rather than a comment.
 *
 * The non-finite cases matter too, and not hypothetically: ppgMetResults.spo2
 * is a ratio of two measured amplitudes and is NaN until the first valid PPG
 * window lands. Converting a NaN to int32_t is undefined behaviour, so those
 * cases are what the UBSan build in tests/CMakeLists.txt is actually guarding
 * -- a regression that dropped the guard would abort here rather than emit a
 * plausible number on the bench.
 */
#include <stdint.h>

#include "obs_fmt.h"
#include "test_assert.h"

int
main(void)
{
    /* volatile so the compiler cannot constant-fold the division and reject it
     * at compile time; these have to be produced the way the firmware produces
     * them, at run time. */
    volatile float zero = 0.0f;
    volatile float one = 1.0f;

    TEST_CASE("positive values round half away from zero");
    CHECK_EQ(hkv_fx2_from_float(0.0f), 0);
    CHECK_EQ(hkv_fx2_from_float(1.0f), 100);
    CHECK_EQ(hkv_fx2_from_float(72.5f), 7250);
    CHECK_EQ(hkv_fx2_from_float(0.005f), 1);

    /* THE REGRESSION. The old idiom returned +50 / +1 / +9999 for these,
     * because the integer part is 0 and it took fabsf() of the remainder. */
    TEST_CASE("sign is preserved for sub-unit negatives");
    CHECK_EQ(hkv_fx2_from_float(-0.5f), -50);
    CHECK_EQ(hkv_fx2_from_float(-0.01f), -1);
    CHECK_EQ(hkv_fx2_from_float(-99.99f), -9999);

    /* Rounding must not bias negatives toward zero the way truncation would. */
    TEST_CASE("rounding is symmetric about zero");
    CHECK_EQ(hkv_fx2_from_float(-0.005f), -1);
    CHECK_EQ(hkv_fx2_from_float(1.234f), 123);
    CHECK_EQ(hkv_fx2_from_float(-1.234f), -123);
    CHECK_EQ(hkv_fx2_from_float(1.236f), 124);
    CHECK_EQ(hkv_fx2_from_float(-1.236f), -124);

    TEST_CASE("out-of-range clamps instead of invoking an undefined cast");
    CHECK_EQ(hkv_fx2_from_float(1.0e30f), 2147483647);
    CHECK_EQ(hkv_fx2_from_float(-1.0e30f), -2147483647);

    /* Infinities are ordered, so they clamp. NaN is not ordered against
     * anything, so it reports the distinct "no reading" sentinel -- and a
     * parser must not read that as a very large negative measurement, which is
     * only true if no real value can reach it. */
    TEST_CASE("non-finite inputs are distinguishable");
    CHECK_EQ(hkv_fx2_from_float(one / zero), 2147483647);
    CHECK_EQ(hkv_fx2_from_float(-one / zero), -2147483647);
    CHECK_EQ(hkv_fx2_from_float(zero / zero), HKV_FX2_INVALID);
    CHECK(hkv_fx2_from_float(-one / zero) != HKV_FX2_INVALID);
    CHECK(hkv_fx2_from_float(-1.0e30f) != HKV_FX2_INVALID);

    return TEST_RESULT();
}
