// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#include "battery_model.h"
#include "test_assert.h"

int main(void)
{
    const hkv_battery_profile_t lp = hkv_battery_profile(false);
    const hkv_battery_profile_t hp = hkv_battery_profile(true);
    CHECK_NEAR(lp.capacity_mwh, 1350.0f, 0.001f);
    CHECK_NEAR(hp.sleep_mw, MCU_SLEEP_POWER_MW, 0.0001f);
    CHECK_NEAR(hp.compute_mw, MCU_COMPUTE_POWER_MW_HP, 0.0001f);
    CHECK_NEAR(hp.denoise_mw, MCU_INFERENCE_POWER_MW_HP, 0.0001f);
    CHECK_NEAR(hp.segment_mw, MCU_INFERENCE_POWER_MW_HP, 0.0001f);
    CHECK_NEAR(hp.arrhythmia_mw, MCU_INFERENCE_POWER_MW_HP, 0.0001f);
#if defined(AM_PART_APOLLO510B)
    CHECK_NEAR(lp.sleep_mw, 1.268f, 0.0001f);
    CHECK_NEAR(lp.compute_mw, 6.0f, 0.0001f);
    CHECK_NEAR(lp.denoise_mw, 8.968f, 0.0001f);
    CHECK_NEAR(lp.segment_mw, 7.443f, 0.0001f);
    CHECK_NEAR(lp.arrhythmia_mw, 8.638f, 0.0001f);
#else
    CHECK_NEAR(lp.sleep_mw, MCU_SLEEP_POWER_MW, 0.0001f);
    CHECK_NEAR(lp.compute_mw, MCU_COMPUTE_POWER_MW_LP, 0.0001f);
    CHECK_NEAR(lp.denoise_mw, MCU_INFERENCE_POWER_MW_LP, 0.0001f);
    CHECK_NEAR(lp.segment_mw, MCU_INFERENCE_POWER_MW_LP, 0.0001f);
    CHECK_NEAR(lp.arrhythmia_mw, MCU_INFERENCE_POWER_MW_LP, 0.0001f);
#endif
    hkv_battery_estimate_t r = hkv_battery_estimate(lp, 0, 0, 0, 0);
    CHECK_NEAR(r.average_mw, lp.sleep_mw / lp.margin, 0.0001f);
    CHECK_NEAR(r.days, 1350.0f * lp.margin / lp.sleep_mw / 24.0f, 0.0001f);
    r = hkv_battery_estimate(lp, 1, 0, 0, 0);
    CHECK_NEAR(r.average_mw, lp.compute_mw / lp.margin, 0.0001f);
    r = hkv_battery_estimate(lp, .13f, .02f, .03f, .0035f);
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
