/**
 * @file store.h
 * @brief Central store for the NSX port (phase 6: full app orchestration).
 *
 * Extends the phase 3 DSP-only store with the pieces needed for full parity
 * with legacy heartkit-vitals-demo's main.cc: app_state_t runtime mode
 * switches (input source, denoise/segmentation/arrhythmia mode, noise
 * levels, CPU speed mode), the app-level CPU/battery metrics struct, the
 * raw+noisy ECG segmentation staging buffers, and the CPU-utilization
 * TileIO TX taps. AI-mode denoise/segmentation/arrhythmia model buffers
 * live in ecg_denoise.h/ecg_segmentation.h/ecg_arrhythmia.h (already
 * ported in phase 4) -- only the orchestration-level globals are added
 * here.
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

///////////////////////////////////////////////////////////////////////////////
// App State
///////////////////////////////////////////////////////////////////////////////

typedef struct {
    float32_t cpuPercUtil;
    float32_t batteryDays;
    float32_t avgAiIps;
    /* Battery-model breakdown (issue #17). Diagnostics only -- these are
     * emitted on the `cpu` HKV report line so the three-state split is
     * observable on SWO, and are NOT part of the TileIO CPU metrics packet
     * (send_cpu_metrics still sends exactly the first three fields). Only the
     * two independent terms are kept: compute and idle are exact derivations
     * of these and cpuPercUtil (see report_extra_cpu). battInferenceFrac is a
     * 0..1 fraction of wall time. */
    float32_t battInferenceFrac;
    float32_t battAvgPowerMw;
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
// Noise-free copy of the denoise input window, kept so EcgProcessTask can
// compute a cosine-similarity "denoise quality" score against the noisy/
// AI-denoised output when running in synthetic (non-live) input mode --
// mirrors legacy's ecgDenNoise.
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

///////////////////////////////////////////////////////////////////////////////
// PPG Metrics Configuration
///////////////////////////////////////////////////////////////////////////////
//
// Phase 6 fix: sensor.c now applies the real dual-wavelength "click golden"
// AS7058 profile (Red PPG1_SUB1 + IR PPG1_SUB2 + ECG) instead of the
// earlier single-wavelength JSON bring-up profile (see sensor.c/
// as7058_profiles.c) -- so metrics_capture_ppg() below now gets two real
// channels and computes a genuine ratiometric SpO2 (via nsx-physiokit's own
// pk_ppg math and the profile's a/b/c + dc_comp_red/ir calibration
// coefficients, exposed via sensor_get_spo2_config() -- no AMS on-chip
// bio_spo2_a0 algorithm needed; that stays a real Cortex-M packaging gap,
// see sensor_get_spo2_config()'s doc comment in sensor.h).

extern rb_config_t rbPpg1Met; /* Red */
extern rb_config_t rbPpg2Met; /* IR */

extern float32_t ppg1MetData[PPG_MET_WINDOW_LEN];
extern float32_t ppg2MetData[PPG_MET_WINDOW_LEN];

extern metrics_ppg_results_t ppgMetResults;

///////////////////////////////////////////////////////////////////////////////
// TileIO Streaming Taps
///////////////////////////////////////////////////////////////////////////////
//
// Separate from the metrics-stage ringbuffers above: these are lightweight
// tap-offs of raw + denoised ECG + QRS mask (from EcgProcessTask's
// preprocessing/segmentation stages) and downsampled dual-wavelength PPG
// samples (from PpgProcessTask), drained by TioProcessTask in main.cc to
// stream live signals to a Tileio host dashboard over nsx-tileio-usb. ECG
// streams raw+denoised+mask (3ch), PPG streams Red+IR (2ch) -- both match
// legacy.

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
