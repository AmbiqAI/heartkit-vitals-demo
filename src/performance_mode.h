// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
#ifndef HKV_PERFORMANCE_MODE_H
#define HKV_PERFORMANCE_MODE_H

#include <stdint.h>

static inline uint8_t
hkv_supported_speed_mode(uint8_t requested)
{
#if defined(AM_PART_APOLLO330P)
    /* HP timing is not qualified for this target; see #71. */
    (void)requested;
    return 0;
#else
    return requested ? 1 : 0;
#endif
}

#endif
