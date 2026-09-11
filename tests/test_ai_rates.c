// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#include "inference_timing.h"
#include "test_assert.h"

int main(void)
{
    volatile float missing = ai_unavailable_rate();
    CHECK(!ai_rate_available(ai_display_rate(4200.0f, false)));
    CHECK(!ai_rate_available(ai_display_rate(missing, true)));
    CHECK(!ai_rate_available(ai_average_rate(missing, missing, missing)));
    const uint32_t infBits = UINT32_C(0x7f800000);
    float infinity;
    memcpy(&infinity, &infBits, sizeof(infinity));
    CHECK(!ai_rate_available(infinity));
    CHECK(!ai_rate_available(-10.0f));
    CHECK_NEAR(ai_average_rate(50.0f, missing, missing), 50.0f, 0.001f);
    CHECK_NEAR(ai_average_rate(50.0f, missing, 100.0f), 75.0f, 0.001f);
    CHECK_NEAR(ai_average_rate(50.0f, 30.0f, 100.0f), 60.0f, 0.001f);
    return TEST_RESULT();
}
