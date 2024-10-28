
/**
 * @file store.h
 * @author Adam Page (adam.page@ambiq.com)
 * @brief Act as central store for app
 * @version 1.0
 * @date 2023-03-27
 *
 * @copyright Copyright (c) 2023
 *
 */
#ifndef __APP_STORE_H
#define __APP_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <arm_math.h>
// Modules
#include "tio_usb.h"
#include "pk_ppg.h"
#include "pk_hrv.h"
#include "pk_ecg.h"
#include "ina228.h"
// neuralSPOT
#include "ns_ambiqsuite_harness.h"
#include "ns_i2c.h"
#include "ns_spi.h"
// #include "ns_peripherals_button.h"
#include "ns_peripherals_power.h"
// Locals
#include "constants.h"
#include "sensor.h"
#include "metrics.h"
#include "ringbuffer.h"
#include "pmic.h"

enum HeartRhythm { HeartRhythmNormal, HeartRhythmAfib, HeartRhythmAfut };
typedef enum HeartRhythm HeartRhythm;

enum HeartBeat { HeartBeatNormal, HeartBeatPac, HeartBeatPvc, HeartBeatNoise };
typedef enum HeartBeat HeartBeat;

enum HeartRate { HeartRateNormal, HeartRateTachycardia, HeartRateBradycardia };
typedef enum HeartRate HeartRate;

enum HeartSegment { HeartSegmentNormal, HeartSegmentPWave, HeartSegmentQrs, HeartSegmentTWave };
typedef enum HeartSegment HeartSegment;

enum DenoiseMode { DenoiseModeOff, DenoiseModeDsp, DenoiseModeAi };
typedef enum DenoiseMode DenoiseMode;

enum SegmentationMode { SegmentationModeOff, SegmentationModeDsp, SegmentationModeAi };
typedef enum SegmentationMode SegmentationMode;

enum ArrhythmiaMode { ArrhythmiaModeOff, ArrhythmiaModeDsp, ArrhythmiaModeAi };
typedef enum ArrhythmiaMode ArrhythmiaMode;

typedef struct {
    float32_t cpuPercUtil;
    float32_t batteryHours;
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

///////////////////////////////////////////////////////////////////////////////
// EVB Configuration
///////////////////////////////////////////////////////////////////////////////

extern ns_power_config_t nsPwrCfg;
extern ns_core_config_t nsCoreCfg;
extern ns_i2c_config_t nsI2cCfg;
extern ns_spi_config_t nsSpiCfg;
// extern ns_button_config_t nsBtnCfg;


///////////////////////////////////////////////////////////////////////////////
// Sensor Configuration
///////////////////////////////////////////////////////////////////////////////

extern sensor_context_t sensorCtx;
extern rb_config_t rbEcgSensor;
extern rb_config_t rbPpg1Sensor;
extern rb_config_t rbPpg2Sensor;

///////////////////////////////////////////////////////////////////////////////
// Preprocess Configuration
///////////////////////////////////////////////////////////////////////////////

extern arm_biquad_casd_df1_inst_f32 ecgFilterCtx;


///////////////////////////////////////////////////////////////////////////////
// ECG Denoise Configuration
///////////////////////////////////////////////////////////////////////////////

extern float32_t ecgDenScratch[ECG_DEN_WINDOW_LEN];
extern float32_t ecgDenInout[ECG_DEN_WINDOW_LEN];
extern float32_t ecgDenNoise[ECG_DEN_WINDOW_LEN];
extern rb_config_t rbEcgDen;

extern float32_t ppg1DenInout[PPG_DEN_WINDOW_LEN];
extern float32_t ppg2DenInout[PPG_DEN_WINDOW_LEN];
extern rb_config_t rbPpg1Den;
extern rb_config_t rbPpg2Den;

///////////////////////////////////////////////////////////////////////////////
// ECG Arrhythmia Configuration
///////////////////////////////////////////////////////////////////////////////

extern float32_t ecgArrScratch[ECG_ARR_WINDOW_LEN];
extern float32_t ecgArrInout[ECG_ARR_WINDOW_LEN];

///////////////////////////////////////////////////////////////////////////////
// ECG Segmentation Configuration
///////////////////////////////////////////////////////////////////////////////

extern float32_t ecgSegScratch[ECG_SEG_WINDOW_LEN];
extern float32_t ecgSegInout[ECG_SEG_WINDOW_LEN];
extern uint16_t ecgSegMask[ECG_SEG_WINDOW_LEN];
extern rb_config_t rbEcgRawSeg;
extern rb_config_t rbEcgSeg;
extern ecg_peak_f32_t ecgPkPeakCtx;

extern float32_t ppg1SegInout[PPG_SEG_WINDOW_LEN];
extern float32_t ppg2SegInout[PPG_SEG_WINDOW_LEN];
extern rb_config_t rbPpg1Seg;
extern rb_config_t rbPpg2Seg;

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

extern rb_config_t rbPpg1Met;
extern rb_config_t rbPpg2Met;

extern float32_t ppg1MetData[PPG_MET_WINDOW_LEN];
extern float32_t ppg2MetData[PPG_MET_WINDOW_LEN];


extern metrics_ppg_results_t ppgMetResults;

///////////////////////////////////////////////////////////////////////////////
// TILEIO Configuration
///////////////////////////////////////////////////////////////////////////////

extern rb_config_t rbEcgRawTx;
extern rb_config_t rbEcgDenTx;
extern uint16_t ecgMaskTxBuffer[ECG_TX_BUF_LEN];
extern rb_config_t rbEcgMaskTx;

extern rb_config_t rbPpg1Tx;
extern rb_config_t rbPpg2Tx;

extern rb_config_t rbEcgCpuTx;
extern rb_config_t rbPpgCpuTx;
extern rb_config_t rbTotalCpuTx;

///////////////////////////////////////////////////////////////////////////////
// APP Configuration
///////////////////////////////////////////////////////////////////////////////

extern metrics_app_results_t appMetResults;

// extern uint8_t LED_COLORS[10][4];
extern ns_timer_config_t ecgTimerCfg;
extern ns_timer_config_t ppgTimerCfg;
extern app_state_t appState;

extern pmic_metrics_results_t g_pmicMetrics;
extern ina228_context_t g_ina228Ctx;

#ifdef __cplusplus
}
#endif

#endif // __APP_STORE_H
