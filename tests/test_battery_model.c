// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#include "battery_model.h"
#include "performance_mode.h"
#include "inference_timing.h"
#include "test_assert.h"

int main(void)
{
    for (unsigned int requested = 0; requested <= UINT8_MAX; ++requested) {
        const uint8_t mode = hkv_supported_speed_mode((uint8_t)requested);
#if defined(AM_PART_APOLLO330P)
        CHECK(mode == 0);
#else
        CHECK(mode == (requested != 0));
#endif
        CHECK_NEAR(hkv_battery_profile(mode != 0).sleep_mw,
                   1.268f, 0.0001f);
    }
    CHECK(isnan(ai_display_rate(4200.0f, false)));
    CHECK(isnan(ai_display_rate(NAN, true)));
    CHECK(isnan(ai_display_rate(0.0f, true)));
    CHECK_NEAR(ai_display_rate(50.0f, true), 50.0f, 0.001f);
    CHECK(isnan(ai_average_rate(NAN, NAN, NAN)));
    CHECK_NEAR(ai_average_rate(50.0f, NAN, NAN), 50.0f, 0.001f);
    CHECK_NEAR(ai_average_rate(50.0f, NAN, 100.0f), 75.0f, 0.001f);
    CHECK_NEAR(ai_average_rate(50.0f, 30.0f, 100.0f), 60.0f, 0.001f);
    CHECK_NEAR(ips_from_delta_us(10000u), 100.0f, 0.001f);
    CHECK_NEAR(ips_from_delta_us(20000u), 50.0f, 0.001f);
    CHECK_NEAR(ips_from_delta_us(0u), 1000000.0f, 0.001f);
    CHECK_NEAR(stage_duty_frac(15u, ips_from_delta_us(20000u), 30.0f), 0.01f, 0.00001f);
    CHECK_NEAR(stage_duty_frac(0u, 50.0f, 30.0f), 0.0f, 0.00001f);
    CHECK_NEAR(stage_duty_frac(1u, 0.0f, 30.0f), 0.0f, 0.00001f);
    CHECK_NEAR(stage_duty_frac(1u, 50.0f, 0.0f), 0.0f, 0.00001f);
    const hkv_battery_profile_t lp = hkv_battery_profile(false);
    const hkv_battery_profile_t hp = hkv_battery_profile(true);
    const float measuredDuty = stage_duty_frac(15u, ips_from_delta_us(20000u), 30.0f);
    const hkv_battery_estimate_t measured = hkv_battery_estimate(lp, .13f, measuredDuty, .03f, .0035f);
    const hkv_battery_estimate_t expectedDuty = hkv_battery_estimate(lp, .13f, .01f, .03f, .0035f);
    CHECK_NEAR(measured.days, expectedDuty.days, 0.0001f);
    CHECK_NEAR(measured.average_mw, expectedDuty.average_mw, 0.0001f);
    CHECK_NEAR(lp.capacity_mwh, 1350.0f, 0.001f);
    CHECK_NEAR(hp.sleep_mw, 1.268f, 0.0001f);
    CHECK_NEAR(hp.compute_mw, 14.622f, 0.0001f);
    CHECK_NEAR(hp.denoise_mw, 22.099f, 0.0001f);
    CHECK_NEAR(hp.segment_mw, 17.614f, 0.0001f);
    CHECK_NEAR(hp.arrhythmia_mw, 21.112f, 0.0001f);
    CHECK_NEAR(hp.margin, 0.80f, 0.0001f);
    CHECK_NEAR(lp.sleep_mw, 1.268f, 0.0001f);
    CHECK_NEAR(lp.compute_mw, 4.623f, 0.0001f);
    CHECK_NEAR(lp.denoise_mw, 6.577f, 0.0001f);
    CHECK_NEAR(lp.segment_mw, 5.393f, 0.0001f);
    CHECK_NEAR(lp.arrhythmia_mw, 6.265f, 0.0001f);
    CHECK_NEAR(lp.margin, 0.80f, 0.0001f);
    CHECK_NEAR(hp.margin, SYSTEM_POWER_MARGIN, 0.0001f);
    hkv_battery_estimate_t r = hkv_battery_estimate(lp, 0, 0, 0, 0);
    CHECK_NEAR(r.average_mw, lp.sleep_mw / lp.margin, 0.0001f);
    CHECK_NEAR(r.days, 1350.0f * lp.margin / lp.sleep_mw / 24.0f, 0.0001f);
    r = hkv_battery_estimate(lp, 1, 0, 0, 0);
    CHECK_NEAR(r.average_mw, lp.compute_mw / lp.margin, 0.0001f);
    r = hkv_battery_estimate(lp, .13f, .02f, .03f, .0035f);
    CHECK_NEAR(r.average_mw, 2.2150962499999998f, 0.0001f);
    CHECK_NEAR(r.days, 25.393930399186946f, 0.001f);
    const float expected = (.02f * lp.denoise_mw + .03f * lp.segment_mw +
        .0035f * lp.arrhythmia_mw + .0765f * lp.compute_mw + .87f * lp.sleep_mw) / lp.margin;
    CHECK_NEAR(r.average_mw, expected, 0.0001f);
    CHECK_NEAR(r.days, 1350.0f / expected / 24.0f, 0.0001f);
    CHECK_NEAR(r.inference_fraction, .0535f, 0.0001f);
    r = hkv_battery_estimate(lp, .2f, .2f, .1f, .1f);
    CHECK_NEAR(r.inference_fraction, .2f, 0.0001f);
    CHECK_NEAR(r.average_mw, (.1f * lp.denoise_mw + .05f * lp.segment_mw +
        .05f * lp.arrhythmia_mw + .8f * lp.sleep_mw) / lp.margin, 0.0001f);
    r = hkv_battery_estimate(lp, -1, .2f, .1f, .1f);
    CHECK_NEAR(r.average_mw, lp.sleep_mw / lp.margin, 0.0001f);
    r = hkv_battery_estimate(lp, 2, -1, 2, 0);
    CHECK_NEAR(r.average_mw, lp.segment_mw / lp.margin, 0.0001f);
    return 0;
}
