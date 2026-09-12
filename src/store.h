// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/** @file store.h
 * @brief Shared application state and signal buffers.
 */
#ifndef __APP_STORE_H
#define __APP_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <arm_math.h>

#include "nsx_i2c.h"
#include "nsx_power.h"
#include "nsx_spi.h"

#include "pk_ecg.h"
#include "pk_hrv.h"
#include "pk_ppg.h"

#include "constants.h"
#include "metrics.h"
#include "sensor.h"
#include "telemetry.h"

///////////////////////////////////////////////////////////////////////////////
// App State
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    float32_t cpuPercUtil;
    float32_t batteryDays;
    float32_t avgAiIps;
    /* Diagnostic-only battery terms; excluded from TileIO metrics.
     * battInferenceFrac is a wall-time fraction in [0,1]. */
    float32_t battInferenceFrac;
    float32_t battAvgPowerMw;
    /* Diagnostic percentages share the measured CPU reporting window. */
    float32_t cpuProjPerc;
    hkv_cpu_split_t cpuSplit;
} metrics_app_results_t;

typedef struct {
    uint8_t inputSource;
    uint8_t bwNoiseLevel; // 0-100
    uint8_t maNoiseLevel; // 0-100
    uint8_t emNoiseLevel; // 0-100
    uint8_t speedMode;  // 0-1
    uint8_t denoiseMode; // 0-2
    uint8_t segMode;  // 0-2
    uint8_t arrMode;  // 0-2
} app_state_t;

extern app_state_t appState;
extern metrics_app_results_t appMetResults;

extern nsx_power_config_t nsxPwrCfg;
extern nsx_i2c_config_t nsxI2cCfg;
extern nsx_spi_config_t nsxSpiCfg;

extern sensor_context_t sensorCtx;

///////////////////////////////////////////////////////////////////////////////
// ECG Preprocess Configuration
///////////////////////////////////////////////////////////////////////////////

extern arm_biquad_casd_df1_inst_f32 ecgFilterCtx;

///////////////////////////////////////////////////////////////////////////////
// ECG Denoise Configuration (DSP-only: biquad bandpass filtfilt)
///////////////////////////////////////////////////////////////////////////////

extern float32_t ecgDenScratch[ECG_DEN_WINDOW_LEN];
extern float32_t ecgDenInout[ECG_DEN_WINDOW_LEN];
// Retain the clean stimulus as the denoise-similarity reference.
extern float32_t ecgDenNoise[ECG_DEN_WINDOW_LEN];
extern rb_config_t rbEcgDen;
// Parallel (non-filtered) raw+noise staging ringbuffer, teed alongside
// rbEcgDen -- feeds the "raw" channel of the 3ch ECG TileIO TX packet.
extern rb_config_t rbEcgRawSeg;

///////////////////////////////////////////////////////////////////////////////
// ECG Segmentation Configuration (DSP-only: pk_ecg_find_peaks_f32)
///////////////////////////////////////////////////////////////////////////////

extern float32_t ecgSegInout[ECG_SEG_WINDOW_LEN];
extern uint16_t ecgSegMask[ECG_SEG_WINDOW_LEN];
extern rb_config_t rbEcgSeg;
extern ecg_peak_f32_t ecgPkPeakCtx;

///////////////////////////////////////////////////////////////////////////////
// Shared Metrics Configuration
///////////////////////////////////////////////////////////////////////////////

extern metrics_config_t metricsCfg;
extern uint32_t peaksMetrics[MAX_RR_PEAKS];
extern uint32_t rriMetrics[MAX_RR_PEAKS];
extern uint8_t rriMask[MAX_RR_PEAKS];

///////////////////////////////////////////////////////////////////////////////
// ECG Metrics Configuration
///////////////////////////////////////////////////////////////////////////////

extern rb_config_t rbEcgMet;
extern rb_config_t rbEcgMaskMet;

extern float32_t ecgMetData[ECG_MET_WINDOW_LEN];
extern uint16_t ecgMaskMetData[ECG_MET_WINDOW_LEN];

extern hrv_td_metrics_t ecgHrvMetrics;

extern metrics_ecg_results_t ecgMetResults;

// PPG metrics buffers.

extern rb_config_t rbPpg1Met; /* Red */
extern rb_config_t rbPpg2Met; /* IR */

extern float32_t ppg1MetData[PPG_MET_WINDOW_LEN];
extern float32_t ppg2MetData[PPG_MET_WINDOW_LEN];

extern metrics_ppg_results_t ppgMetResults;

// Separate TX taps keep transport draining independent of metrics processing.

extern rb_config_t rbEcgRawTx;
extern rb_config_t rbEcgDenTx;
extern rb_config_t rbEcgMaskTx;
extern rb_config_t rbPpg1Tx; /* Red */
extern rb_config_t rbPpg2Tx; /* IR */

///////////////////////////////////////////////////////////////////////////////
// CPU Utilization TileIO Streaming Taps (slot 2)
///////////////////////////////////////////////////////////////////////////////
//
// Per-second ECG/PPG task CPU utilization percentages plus the combined
// total, sampled by CpuProcessTask (main.cc) from FreeRTOS runtime stats
// and streamed to the host dashboard as slot 2. Requires
// configGENERATE_RUN_TIME_STATS=1 (see FreeRTOSConfig.h) and the
// RTOS_AppConfigureTimerForRuntimeStats/GetRuntimeCounterValueFromISR hooks
// (main.cc) backed by an am_hal_timer instance (RTOS_TIMER, constants.h).

extern rb_config_t rbEcgCpuTx;
extern rb_config_t rbPpgCpuTx;
extern rb_config_t rbTotalCpuTx;

#ifdef __cplusplus
}
#endif

#endif // __APP_STORE_H
