// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file test_telemetry.c
 * @brief Host tests for the CPU attribution and duty-cycle projection (src/telemetry.h).
 *
 * The duty factors are checked against an INDEPENDENT derivation from the same
 * pipeline constants (pad and stride rather than the *_VALID_LEN macro the
 * header uses), so a factor built from the wrong pair of constants fails here
 * instead of silently rescaling the projection. See #8.
 */
#include "telemetry.h"
#include "test_assert.h"

#define TOL (1e-5f)

static void
test_duty_factors_derived_from_windows(void)
{
    TEST_CASE("duty factors");

    /* Denoise and segmentation advance by their window less both pads. */
    CHECK_NEAR(HKV_DUTY_ECG_DEN, 1.0f - 2.0f * (float)ECG_DEN_PAD_LEN / (float)ECG_DEN_WINDOW_LEN, TOL);
    CHECK_NEAR(HKV_DUTY_ECG_SEG, 1.0f - 2.0f * (float)ECG_SEG_PAD_LEN / (float)ECG_SEG_WINDOW_LEN, TOL);
    /* Metrics advances by its window less one pad. */
    CHECK_NEAR(HKV_DUTY_ECG_MET, 1.0f - (float)ECG_MET_PAD_LEN / (float)ECG_MET_WINDOW_LEN, TOL);
    /* Arrhythmia has no stride of its own: it runs in the metrics branch, over
     * the first ECG_ARR_WINDOW_LEN samples of the metrics window. Its own
     * *_VALID_LEN equals its window (zero pad) and would read as no overlap. */
    CHECK_NEAR(HKV_DUTY_ECG_ARR, (float)ECG_MET_VALID_LEN / (float)ECG_ARR_WINDOW_LEN, TOL);
    CHECK(HKV_DUTY_ECG_ARR < HKV_DUTY_ECG_DEN);
    CHECK_FEQ(HKV_DUTY_CAPTURE, 1.0f);

    /* Same bound the header static-asserts, checked here on the values rather
     * than the integer operands. */
    CHECK(HKV_DUTY_ECG_DEN > 0.0f && HKV_DUTY_ECG_DEN <= 1.0f);
    CHECK(HKV_DUTY_ECG_SEG > 0.0f && HKV_DUTY_ECG_SEG <= 1.0f);
    CHECK(HKV_DUTY_ECG_ARR > 0.0f && HKV_DUTY_ECG_ARR <= 1.0f);
    CHECK(HKV_DUTY_ECG_MET > 0.0f && HKV_DUTY_ECG_MET <= 1.0f);
}

static void
test_nodemo_subtracts_transport(void)
{
    TEST_CASE("cpu_nodemo");

    CHECK_NEAR(hkv_cpu_nodemo_pct(38.0f, 12.0f), 26.0f, TOL);
    CHECK_NEAR(hkv_cpu_nodemo_pct(38.0f, 0.0f), 38.0f, TOL);
    /* Independent measurements, so transport can exceed measured busy on a
     * skewed window; the figure floors at zero rather than going negative. */
    CHECK_NEAR(hkv_cpu_nodemo_pct(10.0f, 40.0f), 0.0f, TOL);
    CHECK_NEAR(hkv_cpu_nodemo_pct(-5.0f, 3.0f), 0.0f, TOL);
    CHECK_NEAR(hkv_cpu_nodemo_pct(140.0f, 20.0f), 80.0f, TOL);
}

static void
test_projection_scales_inference_only(void)
{
    TEST_CASE("cpu_proj");

    /* Capture is never discounted. */
    CHECK_NEAR(hkv_cpu_proj_pct(7.5f, 0.0f), 7.5f, TOL);

    const float duty = hkv_duty_inference_pct(10.0f, 10.0f, 10.0f);
    CHECK_NEAR(duty, 10.0f * (HKV_DUTY_ECG_DEN + HKV_DUTY_ECG_SEG + HKV_DUTY_ECG_ARR), TOL);
    /* Every stage overlaps, so the projection is strictly below what was
     * measured. A projection at or above the measurement means a duty factor
     * stopped being a discount. */
    CHECK(duty < 30.0f);
    CHECK(duty > 0.0f);
    CHECK_NEAR(hkv_cpu_proj_pct(5.0f, duty), 5.0f + duty, TOL);

    /* A stage that did not run contributes nothing. */
    CHECK_NEAR(hkv_duty_inference_pct(0.0f, 0.0f, 0.0f), 0.0f, TOL);
    CHECK_NEAR(hkv_duty_inference_pct(20.0f, 0.0f, 0.0f), 20.0f * HKV_DUTY_ECG_DEN, TOL);

    CHECK_NEAR(hkv_cpu_proj_pct(90.0f, 50.0f), 100.0f, TOL);
}

static void
test_split_sums_to_wall_time(void)
{
    TEST_CASE("split");

    hkv_cpu_split_t split = hkv_cpu_split(40.0f, 5.0f, 20.0f, 12.0f);
    CHECK_NEAR(split.capture, 5.0f, TOL);
    CHECK_NEAR(split.transport, 12.0f, TOL);
    CHECK_NEAR(split.inference, 20.0f, TOL);
    CHECK_NEAR(split.other, 3.0f, TOL);
    CHECK_NEAR(split.idle, 60.0f, TOL);
    CHECK_NEAR(split.capture + split.inference + split.transport + split.other + split.idle, 100.0f, TOL);

    /* Inference is derived from DWT stage timings, not from the run-time
     * counters the other terms come from, so it can overshoot the headroom.
     * It is capped there rather than pushing `other` negative. */
    split = hkv_cpu_split(30.0f, 4.0f, 40.0f, 6.0f);
    CHECK_NEAR(split.inference, 20.0f, TOL);
    CHECK_NEAR(split.other, 0.0f, TOL);
    CHECK_NEAR(split.capture + split.inference + split.transport + split.other + split.idle, 100.0f, TOL);

    /* Window skew the other way: the two measured tasks together exceed
     * measured busy and are scaled back in proportion. */
    split = hkv_cpu_split(10.0f, 6.0f, 1.0f, 14.0f);
    CHECK_NEAR(split.capture, 3.0f, TOL);
    CHECK_NEAR(split.transport, 7.0f, TOL);
    CHECK_NEAR(split.inference, 0.0f, TOL);
    CHECK_NEAR(split.other, 0.0f, TOL);
    CHECK_NEAR(split.capture + split.inference + split.transport + split.other + split.idle, 100.0f, TOL);

    /* Fully idle board: no component is attributed anything. */
    split = hkv_cpu_split(0.0f, 0.0f, 0.0f, 0.0f);
    CHECK_NEAR(split.idle, 100.0f, TOL);
    CHECK_NEAR(split.other, 0.0f, TOL);
}

static void
test_split_agrees_with_the_three_figures(void)
{
    TEST_CASE("figures agree");

    const float meas = 38.0f;
    const float capture = 5.0f;
    const float transport = 12.0f;
    hkv_cpu_split_t split = hkv_cpu_split(meas, capture, 18.0f, transport);
    const float nodemo = hkv_cpu_nodemo_pct(meas, transport);
    const float proj = hkv_cpu_proj_pct(capture, hkv_duty_inference_pct(6.0f, 6.0f, 6.0f));

    /* The three figures describe the same window and must stay ordered:
     * the projection discounts inference, cpu_nodemo drops only transport. */
    CHECK_NEAR(nodemo, meas - split.transport, TOL);
    CHECK(proj < nodemo);
    CHECK(nodemo < meas);
}

int
main(void)
{
    test_duty_factors_derived_from_windows();
    test_nodemo_subtracts_transport();
    test_projection_scales_inference_only();
    test_split_sums_to_wall_time();
    test_split_agrees_with_the_three_figures();
    return TEST_RESULT();
}
