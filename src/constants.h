// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file constants.h
 * @author Adam Page (adam.page@ambiq.com)
 * @brief Global app constants
 *
 * @copyright Copyright (c) 2024
 *
 */
#ifndef __APP_CONSTANTS_H
#define __APP_CONSTANTS_H


#ifdef __cplusplus
extern "C" {
#endif

// #define APOLLO4_SOC (1)
// #define APOLLO5_SOC (2)
// #define APOLLO_SOC_TYPE APOLLO5_SOC


///////////////////////////////////////////////////////////////////////////////
// Sensor Configuration
///////////////////////////////////////////////////////////////////////////////

#ifdef AM_PART_APOLLO5B
#define LP_CPU_MODE (0)
#define HP_CPU_MODE (2)
#define SPI_IOM (5)
#else
#define LP_CPU_MODE (0)
#define HP_CPU_MODE (2)
#define SPI_IOM (1)
#endif

///////////////////////////////////////////////////////////////////////////////
// Power assumptions for efficiency metrics and battery-profile fallbacks.
///////////////////////////////////////////////////////////////////////////////
// The AP510B LP battery profile is separate from the IPS/W assumptions below.
// Its quiet-idle projection excludes sensor supply power; see
// battery_model.h and AmbiqAI/heartkit-vitals-demo#68.

#if defined(AM_PART_APOLLO510B)

/* Power-reference provenance: see AmbiqAI/heartkit-vitals-demo#18. */
#define MCU_POWER_APOLLO5_FIGURES (1)

#elif defined(AM_PART_APOLLO510)

/* TODO(#71): verify the Apollo510 sleep and compute power references. */
#define MCU_POWER_APOLLO5_FIGURES (1)

#endif

#ifdef MCU_POWER_APOLLO5_FIGURES

/* Sleep-power reference: see AmbiqAI/heartkit-vitals-demo#18. */
#define MCU_SLEEP_POWER_MW (0.75)

/* Compute-power reference: see AmbiqAI/heartkit-vitals-demo#18. */
#define MCU_COMPUTE_UW_PER_MHZ_LP (35.3)
#define MCU_COMPUTE_CLOCK_MHZ_LP  (96.0)
#define MCU_COMPUTE_POWER_MW_LP   (MCU_COMPUTE_UW_PER_MHZ_LP * MCU_COMPUTE_CLOCK_MHZ_LP / 1000.0)

#define MCU_COMPUTE_UW_PER_MHZ_HP (46.8)
#define MCU_COMPUTE_CLOCK_MHZ_HP  (250.0)
#define MCU_COMPUTE_POWER_MW_HP   (MCU_COMPUTE_UW_PER_MHZ_HP * MCU_COMPUTE_CLOCK_MHZ_HP / 1000.0)

/* Shared inference-power reference, not per-model characterization; see AmbiqAI/heartkit-vitals-demo#18. */
#define MCU_INFERENCE_POWER_MW_LP (5.5)
#define MCU_INFERENCE_POWER_MW_HP (16.7)

/* Power allowance is a budgeting assumption; see AmbiqAI/heartkit-vitals-demo#18. */
#define SYSTEM_POWER_MARGIN (0.80)

#else

/* TODO(#71): verify the Apollo330 sleep-power reference. */
#define MCU_SLEEP_POWER_MW (2.12)

/* TODO(#71): characterize Apollo330 inference power. */
#define MCU_INFERENCE_POWER_MW_LP (13.65)
#define MCU_INFERENCE_POWER_MW_HP (13.65)

/* TODO(#71): characterize Apollo330 non-inference compute power. */
#define MCU_COMPUTE_POWER_MW_LP (MCU_INFERENCE_POWER_MW_LP)
#define MCU_COMPUTE_POWER_MW_HP (MCU_INFERENCE_POWER_MW_HP)

/* Power allowance is a budgeting assumption; see AmbiqAI/heartkit-vitals-demo#18. */
#define SYSTEM_POWER_MARGIN (1.0)

#endif

/* Nominal pack energy in mWh, not measured usable energy; see #68.
 * CR2032 nominal voltage: https://data.energizer.com/pdfs/cr2032.pdf */
#define BATT_POWER_CAP (2.0 * 225.0 * 3.0)

#if defined(AM_PART_APOLLO330P)
#define I2C_IOM (2)
#else
#define I2C_IOM (1)
#endif
#define I2C_SPEED_HZ (100000)
#define MAX86150_ADDR (0x5E)
#define LEDSTICK_ADDR (0x23)

#define AS7058_PROFILE_EVK_SPI (0)
#define AS7058_PROFILE_CLICK_I2C (1)

#define AS7058_APP_PROFILE_LEGACY_DEFAULT (0)
#define AS7058_APP_PROFILE_CLICK_PPG_ECG (1)
#define AS7058_APP_PROFILE_CLICK_SPO2 (2)
#define AS7058_APP_PROFILE_CLICK_GOLDEN (3)

#ifndef AS7058_BOARD_PROFILE
#define AS7058_BOARD_PROFILE AS7058_PROFILE_CLICK_I2C
#endif

#if AS7058_BOARD_PROFILE == AS7058_PROFILE_CLICK_I2C
#if defined(AM_PART_APOLLO330P)
#define AS7058_OSAL_INT_PIN 107
#else
#define AS7058_OSAL_INT_PIN 50
#endif
#else
#define AS7058_OSAL_INT_PIN 2
#endif

#ifndef AS7058_BRINGUP_MODE
#define AS7058_BRINGUP_MODE (0)
#endif

#ifndef AS7058_APP_PROFILE
#define AS7058_APP_PROFILE AS7058_APP_PROFILE_CLICK_GOLDEN
#endif

#define AS7058_USE_SPI (AS7058_BOARD_PROFILE == AS7058_PROFILE_EVK_SPI)
#define AS7058_USE_I2C (AS7058_BOARD_PROFILE == AS7058_PROFILE_CLICK_I2C)

#ifndef AS7058_I2C_ADDR
#define AS7058_I2C_ADDR (0x55)
#endif

#ifndef AS7058_I2C_SPEED_HZ
#define AS7058_I2C_SPEED_HZ (400000)
#endif

/* Chiplib register reads go through the IOM command queue and the sensor task
 * blocks until the IOM ISR completes them, instead of spinning in
 * am_hal_iom_blocking_transfer. 0 restores the nsx-i2c blocking read as a
 * fallback. See #65. */
#ifndef HKV_SENSOR_ASYNC
#define HKV_SENSOR_ASYNC (1)
#endif

/* Command queue depth, in 4-byte units. The chiplib issues one read at a time
 * and waits for it, so a single entry would do; the HAL reserves 8 words of
 * header and rounds down to whole entries, so this is the smallest round size
 * that leaves headroom. */
#ifndef HKV_SENSOR_BUS_CQ_WORDS
#define HKV_SENSOR_BUS_CQ_WORDS (256)
#endif

/* Largest single queued read, in bytes. Must track AS7058_FIFO_DATA_BUFFER_SIZE;
 * constants.h stays free of chiplib includes, so sensor_bus.c asserts the two
 * agree. See #65. */
#ifndef HKV_SENSOR_BUS_MAX_READ_BYTES
#define HKV_SENSOR_BUS_MAX_READ_BYTES (1536)
#endif

/* Wire time for that read: 9 bit times per byte (8 data + ack) plus four byte
 * times of addressing overhead (start, write address, register, repeated start
 * and read address). See #65. */
#define HKV_SENSOR_BUS_XFER_MS (((HKV_SENSOR_BUS_MAX_READ_BYTES + 4u) * 9u * 1000u) / AS7058_I2C_SPEED_HZ)

/* A queued read that never completes must not park the sensor task forever;
 * the caller sees a transfer error and the chiplib stops the measurement.
 * 2x the wire time covers clock stretching and scheduler jitter; the floor
 * keeps a short read on a fast bus from timing out on tick granularity. */
#ifndef HKV_SENSOR_BUS_TIMEOUT_MS
#define HKV_SENSOR_BUS_TIMEOUT_MS ((2u * HKV_SENSOR_BUS_XFER_MS) > 20u ? (2u * HKV_SENSOR_BUS_XFER_MS) : 20u)
#endif

#if AS7058_BOARD_PROFILE == AS7058_PROFILE_CLICK_I2C
#define AS7058_LED_SUB1_CFG (0x02) // LED2 (red)
#define AS7058_LED_SUB2_CFG (0x03) // LED3 (IR)
#define AS7058_BOARD_ALLOWED_LED_MASK (0x07) // LEDs 1..3
#else
#define AS7058_LED_SUB1_CFG (34) // LED2 + LED6 (red pair)
#define AS7058_LED_SUB2_CFG (51) // LED3 + LED7 (IR pair)
#define AS7058_BOARD_ALLOWED_LED_MASK (0x77) // LEDs 1,2,3,5,6,7
#endif
// PD2, PD3, PD5 are physically connected on both EVK and Click variants used here.
#define AS7058_BOARD_ALLOWED_PD_MASK (0x16)

#ifndef EN_SPO2_ALGO
#define EN_SPO2_ALGO (1)
#endif

#ifndef EN_RRM_ALGO
#define EN_RRM_ALGO (0)
#endif

#ifndef EN_AS7058_IIR
#define EN_AS7058_IIR (0)
#endif

#ifndef EN_AS7058_CB_DEBUG_LOGS
#define EN_AS7058_CB_DEBUG_LOGS (0)
#endif

// Diagnostic output controls; counters remain enabled independently.

/* Reporting is independent of counter collection; see AmbiqAI/heartkit-vitals-demo#11. */
#ifndef EN_APP_REPORT
#define EN_APP_REPORT (1)
#endif

/* Trace output can perturb timing; see AmbiqAI/heartkit-vitals-demo#11. */
#ifndef EN_APP_TRACE
#define EN_APP_TRACE (0)
#endif

#ifndef EN_MODEL_VERBOSE_LOGS
#define EN_MODEL_VERBOSE_LOGS (0)
#endif

#define NUM_INPUT_PTS (6)
#define LIVE_INPUT_MODE NUM_INPUT_PTS
#define PTS_ECG_DATA_LEN (4000)
#define PTS_PPG_DATA_LEN (2000)

#define SENSOR_BUF_LEN (4 * 64)
#define AS7058_SENSOR_TASK_STACK_WORDS (1024)
#define AS7058_SENSOR_TASK_PRIORITY (2)

/* The chiplib stops the measurement when a FIFO read fails, so the sensor task
 * cannot wait on the INT notification alone: it would never wake again. See
 * #67. */
#define AS7058_SENSOR_TASK_POLL_MS (250)
#define AS7058_RESTART_INTERVAL_MS (1000)
#define AS7058_RESTART_MAX_FAILURES (5)
/* Spacing once the consecutive-failure budget is spent: long enough that a
 * sensor that is not coming back costs almost nothing, short enough that one
 * that does (cable reseated, supply settled) recovers without a reset. */
#define AS7058_RESTART_BACKOFF_MS (30000)

///////////////////////////////////////////////////////////////////////////////
// Preprocess Configuration
///////////////////////////////////////////////////////////////////////////////

#define NORM_STD_EPS (0.001)

#define ECG_SOS_LEN (3)
#define ECG_SAMPLE_RATE (200)
#define ECG_TARGET_RATE (100)
#define ECG_DS_RATE (ECG_SAMPLE_RATE / ECG_TARGET_RATE)

#define PPG_SAMPLE_RATE (100)
#define PPG_TARGET_RATE (100)
#define PPG_DS_RATE (PPG_SAMPLE_RATE / PPG_TARGET_RATE)
// Click-board AGC bring-up tuning: narrower/lower target band to reduce oscillation and clipping swings.
#define PPG_AGC_MIN (180000)
#define PPG_AGC_MAX (620000)
// PPG display conditioning is TX-only; metrics retain the unmodified samples.
#define PPG_TX_GAIN (1.0f)
#define PPG_TX_BASELINE_ALPHA (0.01f)
#define PPG_TX_STEP_THRESHOLD (750.0f)
// Additional synthetic Gaussian noise (std-dev in ADC counts) for non-live PPG playback.
#define PPG_STIM_GAUSS_STD (50.0f)

///////////////////////////////////////////////////////////////////////////////
// ECG Denoise Configuration
///////////////////////////////////////////////////////////////////////////////

#define ECG_DEN_MODEL_SIZE_KB (140)
#define ECG_DEN_THRESHOLD (0.5)
// 256 matches the deployed model's time axis, so the window carries no padded tail. See #36.
#define ECG_DEN_WINDOW_LEN (256)
#define ECG_DEN_PAD_LEN (25)
#define ECG_DEN_VALID_LEN (ECG_DEN_WINDOW_LEN - 2 * ECG_DEN_PAD_LEN)
#define ECG_DEN_BUF_LEN (2 * ECG_DEN_WINDOW_LEN)

///////////////////////////////////////////////////////////////////////////////
// ECG Segmentation Configuration
///////////////////////////////////////////////////////////////////////////////

/* Only the TFLM reference in the parity runner still allocates this arena: the
 * firmware runs segmentation on the heliaAOT module, whose arena is planned at
 * generation time. See tools/aot/parity/tflm_ref.cc. */
#define ECG_SEG_MODEL_SIZE_KB (145)
#define ECG_SEG_THRESHOLD (0.5)
#define ECG_SEG_NUM_CLASS (4) // 2
// 256 matches the deployed model's time axis, so the window carries no padded tail. See #36.
#define ECG_SEG_WINDOW_LEN (256)
#define ECG_SEG_PAD_LEN (25)
#define ECG_SEG_VALID_LEN (ECG_SEG_WINDOW_LEN - 2 * ECG_SEG_PAD_LEN)
#define ECG_SEG_BUF_LEN (2 * ECG_SEG_WINDOW_LEN)


// ECG Segmentation Classes
#define ECG_SEG_NONE (0)
#define ECG_SEG_PWAVE (1)
#define ECG_SEG_QRS (2)
#define ECG_SEG_TWAVE (3)

///////////////////////////////////////////////////////////////////////////////
// ECG Arrhythmia Configuration
///////////////////////////////////////////////////////////////////////////////

/* Parity-runner only; see the note on ECG_SEG_MODEL_SIZE_KB. */
#define ECG_ARR_MODEL_SIZE_KB (40)
#define ECG_ARR_THRESHOLD (0.4)
#define ECG_ARR_WINDOW_LEN (500)
#define ECG_ARR_PAD_LEN (0)
#define ECG_ARR_VALID_LEN (ECG_ARR_WINDOW_LEN - 2 * ECG_ARR_PAD_LEN)
#define ECG_ARR_BUF_LEN (2 * ECG_ARR_WINDOW_LEN)

// ECG Arrhythmia Classes
#define ECG_ARR_INCONCLUSIVE (0)
#define ECG_ARR_SR (1)
#define ECG_ARR_SB (2)
#define ECG_ARR_AFIB (3)
#define ECG_ARR_GSVT (4)

///////////////////////////////////////////////////////////////////////////////
// PPG Denoise Configuration
///////////////////////////////////////////////////////////////////////////////

#define PPG_DEN_PAD_LEN ECG_DEN_PAD_LEN
#define PPG_DEN_WINDOW_LEN ECG_DEN_WINDOW_LEN
#define PPG_DEN_VALID_LEN (PPG_DEN_WINDOW_LEN - 2 * PPG_DEN_PAD_LEN)
#define PPG_DEN_BUF_LEN (2 * PPG_DEN_WINDOW_LEN)

///////////////////////////////////////////////////////////////////////////////
// PPG Segmentation Configuration
///////////////////////////////////////////////////////////////////////////////

#define PPG_SEG_WINDOW_LEN ECG_DEN_WINDOW_LEN
#define PPG_SEG_PAD_LEN ECG_DEN_PAD_LEN
#define PPG_SEG_VALID_LEN (PPG_SEG_WINDOW_LEN - 2 * PPG_SEG_PAD_LEN)
#define PPG_SEG_BUF_LEN (2 * PPG_SEG_WINDOW_LEN)


///////////////////////////////////////////////////////////////////////////////
// TIO ECG Mask Format
///////////////////////////////////////////////////////////////////////////////

#define TIO_UIO_INPUT_SEL_IDX (0)
#define TIO_UIO_BW_NOISE_IDX (1)
#define TIO_UIO_MA_NOISE_IDX (2)
#define TIO_UIO_EM_NOISE_IDX (3)
#define TIO_UIO_SPEED_MODE_IDX (4)
#define TIO_UIO_DEN_MODE_IDX (5)
#define TIO_UIO_SEG_MODE_IDX (6)
#define TIO_UIO_ARR_MODE_IDX (7)

// TIO Mask Format
// [5-0] : 6-bit segmentation
// [7-6] : 2-bit QoS (0:bad, 1:poor, 2:fair, 3:good)
// [15-8] : 8-bit Fiducial

#define SIG_MASK_SEG_OFFSET (0)
#define SIG_MASK_SEG_MASK (0x3F)
#define SIG_MASK_QOS_OFFSET (6)
#define SIG_MASK_QOS_MASK (0x3)
#define SIG_MASK_FID_OFFSET (8)
#define SIG_MASK_FID_MASK (0xFF)

// SIG QoS Classes
#define SIG_QOS_BAD (0)
#define SIG_QOS_POOR (1)
#define SIG_QOS_FAIR (2)
#define SIG_QOS_GOOD (3)

// ECG Mask
// [5-0] : 6-bit segmentation (0:none, 1:p-wave, 2:qrs, 3:t-wave)
// [7-6] : 2-bit QoS (0:bad, 1:poor, 2:fair, 3:good)
// [9-8] : 2-bit fiducial (0:none, 1:p-peak, 2:qrs, 3:t-peak)
// [15-10] : 6-bit beat type (0:none, 1:nsr, 2:pac/pvc, 3:noise)

#define ECG_MASK_SEG_OFFSET (0)
#define ECG_MASK_SEG_MASK (0x3F)
#define ECG_MASK_QOS_OFFSET (6)
#define ECG_MASK_QOS_MASK (0x3)

#define ECG_QOS_GOOD_THRESH (0.65)
#define ECG_QOS_FAIR_THRESH (0.60)
#define ECG_QOS_POOR_THRESH (0.55)
#define ECG_QOS_BAD_AVG_THRESH (0.70)

#define ECG_MASK_FID_PEAK_OFFSET (8)
#define ECG_MASK_FID_PEAK_MASK (0x3)
#define ECG_MASK_FID_BEAT_OFFSET (10)
#define ECG_MASK_FID_BEAT_MASK (0x3F)

// ECG Fiducial Peak Classes
#define ECG_FID_PEAK_NONE (0)
#define ECG_FID_PEAK_PPEAK (1)
#define ECG_FID_PEAK_QRS (2)
#define ECG_FID_PEAK_TPEAK (3)

// ECG Fiducial Beat Classes
#define ECG_FID_BEAT_NONE (0)
#define ECG_FID_BEAT_NSR (1)
#define ECG_FID_BEAT_PNC (2)
#define ECG_FID_BEAT_NOISE (3)


///////////////////////////////////////////////////////////////////////////////
// Metrics Configuration
///////////////////////////////////////////////////////////////////////////////

#define MIN_RR_SEC (0.3)
#define MAX_RR_SEC (2.0)
#define MIN_RR_DELTA (0.3)
#define MET_CAPTURE_SEC (10)
#define MAX_RR_PEAKS (100 * MET_CAPTURE_SEC)

#define ECG_MET_WINDOW_LEN (MET_CAPTURE_SEC * ECG_TARGET_RATE)
#define ECG_MET_PAD_LEN (8 * ECG_TARGET_RATE)
#define ECG_MET_VALID_LEN (ECG_MET_WINDOW_LEN - ECG_MET_PAD_LEN)
#define ECG_MET_BUF_LEN (2 * ECG_MET_WINDOW_LEN)

#define ECG_TX_BUF_LEN ECG_SEG_BUF_LEN

#define PPG_MET_WINDOW_LEN ECG_MET_WINDOW_LEN
#define PPG_MET_PAD_LEN ECG_MET_PAD_LEN
#define PPG_MET_VALID_LEN (PPG_MET_WINDOW_LEN - PPG_MET_PAD_LEN)
#define PPG_MET_BUF_LEN (2 * PPG_MET_WINDOW_LEN)

#define PPG_TX_BUF_LEN ECG_TX_BUF_LEN

///////////////////////////////////////////////////////////////////////////////
// Tileio Configuration
///////////////////////////////////////////////////////////////////////////////

#define TIO_BLE_ENABLED true // Enable Tileio BLE
#define TIO_USB_ENABLED true // Enable Tileio USB

/* Absorb host jitter with bounded storage; see AmbiqAI/heartkit-vitals-demo#12. */
#define TIO_TX_QUEUE_DEPTH (48)

/* Queue occupancy at which a held USB packet stops gating the drain. Past it
 * the head is released to the second transport and charged to USB as a drop,
 * so a stalled host bounds BLE delay at this depth instead of at the stall
 * length; the remaining third of the queue is the resume margin. See #56. */
#define TIO_TX_USB_HOLD_WATERMARK ((TIO_TX_QUEUE_DEPTH * 2) / 3)

#define TIO_SLOT0_NUM_CH (2)
#define TIO_SLOT0_SIG_NUM_VALS (10)
#define TIO_SLOT0_FS (ECG_TARGET_RATE / TIO_SLOT0_SIG_NUM_VALS)
#define TIO_SLOT0_SCALE (1000)

// Bound buffering and emission independently; see AmbiqAI/heartkit-vitals-demo#12.

/* Host-visible jitter bound; see AmbiqAI/heartkit-vitals-demo#12. */
#define TIO_JITTER_BUDGET_MS (250)

/* Drain produced samples without replaying missed periods; see AmbiqAI/heartkit-vitals-demo#12. */

/* Pump period for the ECG and PPG signal slots. At configTICK_RATE_HZ = 1000
 * this is exactly 100 ticks, so pdMS_TO_TICKS() is exact and the pump can be
 * paced with vTaskDelayUntil() without rounding drift. */
#define TIO_PUMP_INTERVAL_MS (100)

/* Stagger signal pumps to reduce simultaneous enqueues. */
#define TIO_PPG_PUMP_PHASE_MS (33)

/* Packet sizes follow the target sample rate and pump interval. */
#define TIO_ECG_SAMPLES_PER_PKT (10)
#define TIO_PPG_SAMPLES_PER_PKT (10)

/* Bound drift correction per tick to avoid catch-up bursts. */
#define TIO_TX_DRIFT_CATCHUP_SAMPLES (1)

/* Reserve capacity for a producer block and scheduling slip. */
#define TIO_ECG_TX_TROUGH_TARGET (2 * TIO_ECG_SAMPLES_PER_PKT)
#define TIO_PPG_TX_TROUGH_TARGET (2 * TIO_PPG_SAMPLES_PER_PKT)

/* Span producer blocks to reject block-phase aliasing; see AmbiqAI/heartkit-vitals-demo#12. */
#define TIO_TX_SERVO_WINDOW_TICKS (64)

/* Damp position correction relative to drift-rate correction. */
#define TIO_TX_SERVO_PULL_DIV (8)

/* Bound drift compensation to prevent unbounded bursts. */
#define TIO_TX_SERVO_MAX_BUDGET (32)

/* Largest packet either signal slot can emit -- sizes the sender stack buffers
 * and must account for the drift drain above, not just the nominal size. */
#define TIO_ECG_MAX_SAMPLES_PER_PKT (TIO_ECG_SAMPLES_PER_PKT + TIO_TX_DRIFT_CATCHUP_SAMPLES)
#define TIO_PPG_MAX_SAMPLES_PER_PKT (TIO_PPG_SAMPLES_PER_PKT + TIO_TX_DRIFT_CATCHUP_SAMPLES)

/* Bound queued signal age while allowing producer blocks; see AmbiqAI/heartkit-vitals-demo#12. */

/* Backstop tolerance when judging a bench capture, in samples/s. Expect 0.
 * 0.5% of a 100 Hz stream is 0.5 samples/s; round up to 1. */
#define TIO_TX_TRIM_DRIFT_ALLOWANCE_SPS (1)

/* Reserve headroom for pump jitter rounded to whole packet intervals. */
#define TIO_TX_SLACK_SAMPLES (40)

/* Include producer scheduling slip in the occupancy bound. */
#define TIO_TX_SLIP_SAMPLES (30)

/* ECG structural block: the segmentation branch is the sole producer of the
 * ECG TX taps and it pushes one whole ECG_SEG_VALID_LEN batch per cycle
 * (main.cc, ECG SEGMENTATION). See #36. */
#define TIO_ECG_TX_BLOCK_SAMPLES (ECG_SEG_VALID_LEN)
#define TIO_ECG_TX_HIGH_WATER                                                                                          \
    (TIO_ECG_TX_BLOCK_SAMPLES + TIO_ECG_SAMPLES_PER_PKT + TIO_TX_SLACK_SAMPLES + TIO_TX_SLIP_SAMPLES)

/* Reserve capacity for a sensor FIFO burst. */
#define TIO_PPG_TX_BLOCK_SAMPLES (13)
#define TIO_PPG_TX_HIGH_WATER                                                                                          \
    (TIO_PPG_TX_BLOCK_SAMPLES + TIO_PPG_SAMPLES_PER_PKT + TIO_TX_SLACK_SAMPLES + TIO_TX_SLIP_SAMPLES)


///////////////////////////////////////////////////////////////////////////////
// APP Configuration
///////////////////////////////////////////////////////////////////////////////

#define RTOS_TIMER (4)

///////////////////////////////////////////////////////////////////////////////
// App Mode Enums (input source, denoise/segmentation/arrhythmia mode select)
///////////////////////////////////////////////////////////////////////////////

enum DenoiseMode { DenoiseModeOff, DenoiseModeDsp, DenoiseModeAi };
typedef enum DenoiseMode DenoiseMode;

enum SegmentationMode { SegmentationModeOff, SegmentationModeDsp, SegmentationModeAi };
typedef enum SegmentationMode SegmentationMode;

enum ArrhythmiaMode { ArrhythmiaModeOff, ArrhythmiaModeDsp, ArrhythmiaModeAi };
typedef enum ArrhythmiaMode ArrhythmiaMode;

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#define MIN3(a, b, c) (MIN(MIN(a, b), c))
#define MIN4(a, b, c, d) (MIN(MIN(a, b), MIN(c, d)))
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define MAX3(a, b, c) (MAX(MAX(a, b), c))
#define MAX4(a, b, c, d) (MAX(MAX(a, b), MAX(c, d)))
#define CLIP(a, min, max) (MAX(MIN(a, max), min))

#ifdef __cplusplus
}
#endif

#endif // __APP_CONSTANTS_H
