/**
 * @file store.h
 * @brief Central store for the NSX port (phase 3: DSP metrics pipeline).
 *
 * Subset of the legacy heartkit-vitals-demo store.h: board/bus configuration
 * (phase 2) plus the ECG/PPG DSP preprocessing + metrics buffers needed to
 * compute HR/HRV (ECG) and PR/QoS (PPG) purely via nsx-physiokit, with no AI
 * dependency (heliaRT ML integration is a later phase). AI-mode-only
 * globals (denoise/segmentation/arrhythmia model buffers, PMIC, TileIO
 * streaming, app_state_t mode switches) are intentionally still omitted —
 * only the DSP code path in the legacy app is exercised for now.
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
extern rb_config_t rbEcgDen;

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
// tap-offs of already-computed denoised ECG + QRS mask (from EcgProcessTask's
// segmentation stage) and downsampled PPG samples (from PpgProcessTask),
// drained by TioTxTask in main.cc to stream live signals to a Tileio host
// dashboard over nsx-tileio-usb. Unlike legacy (which tees raw+denoised+mask
// 3-wide for ECG and dual-wavelength for PPG), this only streams
// denoised+mask for ECG (2ch) and the single available PPG wavelength (1ch)
// -- matching the same single-wavelength sensor profile limitation
// documented in the PPG metrics section above.

extern rb_config_t rbEcgTx;
extern rb_config_t rbEcgMaskTx;
extern rb_config_t rbPpg1Tx;

#ifdef __cplusplus
}
#endif

#endif // __APP_STORE_H
