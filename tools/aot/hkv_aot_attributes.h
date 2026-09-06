// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
//
// Force-included into the generated heliaAOT modules via
// <MODULE>_ATTRIBUTES_HEADER (see the app CMakeLists.txt), ahead of the
// #ifndef guards in each module's platform.h.
//
// Puts the AOT scratch arenas in the same shared SRAM section as the TFLM
// tensor arenas in src/ecg_*.cc, which use AM_SHARED_RW. Spelled out here
// rather than including am_hal_global.h so the generated modules keep their
// SDK-free include set. `.shared` is defined by the apollo510b, apollo510 and
// apollo330P linker scripts. See AmbiqAI/heartkit-vitals-demo#37.
#ifndef HKV_AOT_ATTRIBUTES_H
#define HKV_AOT_ATTRIBUTES_H

#define HKV_SEGMENTATION_PUT_IN_SRAM __attribute__((section(".shared")))
#define HKV_ARRHYTHMIA_PUT_IN_SRAM   __attribute__((section(".shared")))

#endif // HKV_AOT_ATTRIBUTES_H
