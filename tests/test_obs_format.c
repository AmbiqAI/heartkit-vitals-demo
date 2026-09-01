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

    TEST_CASE("counter delta is ordinary in the common case");
    CHECK_EQ(hkv_counter_delta(0u, 0u), 0u);
    CHECK_EQ(hkv_counter_delta(10u, 0u), 10u);
    CHECK_EQ(hkv_counter_delta(1010u, 1000u), 10u);

    /* THE LOAD-BEARING CASE. obs.h calls the wrap-correct delta "the whole
     * reason counters are not resettable": it is what lets a counter run free
     * and lets each consumer keep its own snapshot. If this is ever changed to
     * signed arithmetic the first report after a wrap emits ~4.29e9 into a
     * `_ps` field and nothing else in the firmware notices. */
    TEST_CASE("counter delta is correct across the 32-bit wrap");
    CHECK_EQ(hkv_counter_delta(0u, 0xFFFFFFFFu), 1u);
    CHECK_EQ(hkv_counter_delta(9u, 0xFFFFFFFFu), 10u);
    CHECK_EQ(hkv_counter_delta(0x00000005u, 0xFFFFFFFBu), 10u);
    /* Straddling the wrap must not produce a huge value. A per-second rate on
     * this app's busiest counter is order 100, so anything above a few
     * thousand here is the signed-arithmetic regression. */
    CHECK(hkv_counter_delta(4u, 0xFFFFFF00u) < 1000u);
    /* Exactly one full wrap reads as zero elapsed. That is unavoidable at any
     * width and is not what this test is defending against; it is pinned so
     * the boundary behaviour is stated rather than discovered. */
    CHECK_EQ(hkv_counter_delta(0x1234u, 0x1234u), 0u);

    TEST_CASE("gauge sentinel is hidden from the wire");
    CHECK_EQ(hkv_gauge_lo_display(HKV_GAUGE_LO_INIT), 0u);
    /* Every other value passes through untouched -- including 0, which is a
     * legitimate observation of an empty ring and must not be confused with
     * the sentinel. The `_n` field, not this function, is what tells those two
     * apart on the wire. */
    CHECK_EQ(hkv_gauge_lo_display(0u), 0u);
    CHECK_EQ(hkv_gauge_lo_display(1u), 1u);
    CHECK_EQ(hkv_gauge_lo_display(280u), 280u);
    CHECK_EQ(hkv_gauge_lo_display(0xFFFFFFFEu), 0xFFFFFFFEu);

    return TEST_RESULT();
}
