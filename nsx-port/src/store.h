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
// NOTE: the AS7058 "click_ppg_ecg" profile currently applied
// (as7058_profiles.c) only enables one PPG wavelength (PPG1_SUB1,
// ppg1_sub_en=1) -- there is no second wavelength to ratio against, so
// SpO2 is not physiologically derivable from raw DSP here. metrics_capture_ppg()
// is still called (with the single channel duplicated into both ppg1/ppg2
// slots) to get pulse rate (PR) and signal quality (QoS), but its `spo2`
// output is discarded/marked n/a by the caller. Real SpO2 requires either a
// dual-wavelength profile (e.g. click_spo2, which has no ECG) or the AMS
// on-chip bio_spo2_a0 algorithm (bundled in nsx-as7058, but currently only
// packaged as an x86-64 Windows lib -- no Cortex-M lib shipped -- a gap to
// revisit, similar in spirit to the helia-dsp arch-flags fix).

extern rb_config_t rbPpg1Met;

extern float32_t ppg1MetData[PPG_MET_WINDOW_LEN];

extern metrics_ppg_results_t ppgMetResults;

///////////////////////////////////////////////////////////////////////////////
// TileIO Streaming Taps
///////////////////////////////////////////////////////////////////////////////
//
// Separate from the metrics-stage ringbuffers above: these are lightweight
// tap-offs of raw + denoised ECG + QRS mask (from EcgProcessTask's
// preprocessing/segmentation stages) and downsampled PPG samples (from
// PpgProcessTask), drained by TioProcessTask in main.cc to stream live
// signals to a Tileio host dashboard over nsx-tileio-usb. ECG streams
// raw+denoised+mask (3ch), matching legacy. PPG streams only the single
// available wavelength (1ch), matching the single-wavelength sensor
// profile limitation documented in the PPG metrics section above.

extern rb_config_t rbEcgRawTx;
extern rb_config_t rbEcgDenTx;
extern rb_config_t rbEcgMaskTx;
extern rb_config_t rbPpg1Tx;

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
