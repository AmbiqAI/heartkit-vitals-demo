// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file constants.h
 * @author Adam Page (adam.page@ambiq.com)
 * @brief Global app constants
 * @version 1.0
 * @date 2024-09-16
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
// Battery / power model (issue #17; figures verified in issue #18)
///////////////////////////////////////////////////////////////////////////////
//
// WHAT THIS MODELS -- MCU ENERGY ONLY. SENSOR POWER IS DELIBERATELY EXCLUDED.
// Sensor power depends on LED count, drive strength and sampling duty, none of
// which are properties of the MCU. Excluding it keeps the figure honest about
// what is being claimed and comparable across use cases. Anywhere this number
// is displayed it must read "MCU energy only, sensor excluded; estimated from
// datasheet and bench figures" or it will be quoted as a product spec.
//
// THREE STATES. Time is split into inference / general compute / sleep, and
// each is billed at its own figure (see main.cc CpuProcessTask). The previous
// two-state model billed all busy time -- transport, DSP, ring copies -- at
// inference power.
//
// THE OPERATING POINT IS A RUNTIME CONTROL, SO THE FIGURES COME IN PAIRS.
// `appState.speedMode` is exposed on the dashboard UIO surface next to the
// battery tile (TIO_UIO_SPEED_MODE_IDX) and switches the SoC between
// NSX_POWER_PERF_LOW (96 MHz) and NSX_POWER_PERF_HIGH (250 MHz) at runtime
// (main.cc set_speed_mode). Every busy-state figure below therefore exists in
// an LP and an HP variant and CpuProcessTask selects the pair that matches the
// live speed mode. A single LP-only set would move the tile the WRONG way in HP
// mode: HP finishes the same work ~2.6x faster, so measured busy time and the
// derived inference duty both fall, and billing that smaller busy fraction at
// LP power reports MORE battery life at ~3x the actual draw. The sleep figure
// is shared: System Sleep 1 does not depend on the run clock.
//
// THE SLEEP TERM IS A DEPLOYMENT PROJECTION, NOT A MEASUREMENT OF THIS BUILD.
// The demo does not sleep: FreeRTOSConfig.h sets `configUSE_TICKLESS_IDLE 0`,
// so idle time is spent spinning in the idle task at run power, not in Sleep 1.
// The idle term states what the same workload would draw if the port slept.
// The busy fraction, by contrast, IS measured: `cpuPercUtil`, the 30 s rolling
// utilisation, so everything the core runs is billed at active power. See #65.
//
// SLEEP 1, NOT DEEP SLEEP, DELIBERATELY. The sensor wakes the MCU ~7.7 times
// per second. Sleep 1 keeps HFRC running so a wake is cheap; deep sleep at that
// cadence pays an HFRC restart per wake, and the datasheet publishes no wake
// energy, so its 36 uW steady-state figure is not the whole cost. Sleep 1 is
// both the conservative and the honest choice at this wake rate.
//
// WHERE THE INFERENCE FIGURES ARE OPTIMISTIC -- STATED, NOT BURIED. Each
// inference figure is the LOWER of the two models that were actually measured:
// LP 5.5 mW is segmentation (5.496) and not arrhythmia (5.743), ~4.5%
// optimistic against the higher measurement; HP 16.7 mW is segmentation
// (16.697) and not arrhythmia (17.698), ~5.7% optimistic on the same basis.
// The selection rule is deliberate and identical in both sets -- segmentation
// is the stage that runs most often -- but it is a choice, not an average.
// Separately, ecg_denoise, which is the MOST FREQUENTLY EXECUTED stage and the
// largest single contributor to inference duty, has NO power measurement in
// either column; it is billed at the segmentation figure. If denoise is more
// expensive than segmentation, this model understates power and overstates
// battery days, and nothing in the code can detect that.
//
// METHODOLOGY NOTE -- MIXED MEASUREMENT DOMAINS, STATED NOT BLENDED. The
// datasheet figures below are SoC-only at VDD 1.8 V. The inference figures are
// apollo510_evb BOARD-LEVEL measurements. Mixing the two is acceptable at demo
// precision, where order-of-magnitude is the bar, but it is a known
// inconsistency and is recorded here rather than hidden in the arithmetic.

#if defined(AM_PART_APOLLO510B)

/* apollo510b_evb. Datasheet figures below are native to this part; the bench
 * figures were taken on apollo510_evb (see the runlog citation), which is the
 * cross-part borrow in this direction and is the smaller of the two risks. */
#define MCU_POWER_APOLLO5_FIGURES (1)

#elif defined(AM_PART_APOLLO510)

/* apollo510_evb (Apollo510 non-B). This part defines AM_PART_APOLLO5B and
 * AM_PART_APOLLO510 but NOT AM_PART_APOLLO510B, so the old `#ifdef
 * AM_PART_APOLLO5B` silently gave it Apollo510B provenance. It now has its own
 * branch and uses the same numbers with an explicit caveat:
 *   - The two INFERENCE figures are NATIVE to this board: the runlog cited
 *     below is apollo510_evb.
 *   - The SLEEP and COMPUTE-per-MHz figures are APOLLO510B DATASHEET VALUES
 *     used here as a cross-part placeholder. They are not verified for the
 *     non-B part.
 * TODO(verify): the Apollo510 (non-B) SoC datasheet exists at
 * OneDrive .../Ambiq/sws/datasheets/soc/ap5/Apollo510-SoC-Datasheet.pdf. Read
 * Table 39 "Current Consumption in Active Mode and Sleep Modes" for ISS1
 * (System Sleep 1), IRUNLPFB and IRUNHPFB, and split this branch out with its
 * own numbers if they differ. Deliberately NOT transcribed here: nobody has
 * opened that document for this change. */
#define MCU_POWER_APOLLO5_FIGURES (1)

#endif

#ifdef MCU_POWER_APOLLO5_FIGURES

/* Idle/sleep power. Shared by both speed modes -- Sleep 1 gates the core
 * clocks, so the figure does not depend on the run clock.
 * Source: Apollo510B SoC Datasheet DS-A510B-1p1p0, Table 39 "Current
 *         Consumption in Active Mode and Sleep Modes", symbol ISS1
 *         (System Sleep 1, 160 kB TCM retained), p.216, 2026.
 * Conditions: WFI SLEEP=1, clocks gated, HFRC on, XTAL off, buck enabled,
 *         NVM standby, cache retained, VDD 1.8 V. Typical, 750 uW. */
#define MCU_SLEEP_POWER_MW (0.75)

/* General compute (non-inference busy time: DSP, transport, ring copies).
 * Kept as the per-MHz figure and the clock so the derivation stays visible
 * instead of collapsing to a magic number.
 * Source: Apollo510B SoC Datasheet DS-A510B-1p1p0, Table 39, symbols IRUNLPFB
 *         and IRUNHPFB (CoreMark run power, low-power / high-performance
 *         mode), p.216, 2026.
 * Conditions: MRAM, cache enabled, buck enabled, VDD 1.8 V. Typical.
 * Clocks: 96 MHz for NSX_POWER_PERF_LOW, 250 MHz for NSX_POWER_PERF_HIGH --
 *         the two operating points appState.speedMode selects between. */
#define MCU_COMPUTE_UW_PER_MHZ_LP (35.3)
#define MCU_COMPUTE_CLOCK_MHZ_LP  (96.0)
#define MCU_COMPUTE_POWER_MW_LP   (MCU_COMPUTE_UW_PER_MHZ_LP * MCU_COMPUTE_CLOCK_MHZ_LP / 1000.0) // 3.389 mW

#define MCU_COMPUTE_UW_PER_MHZ_HP (46.8)
#define MCU_COMPUTE_CLOCK_MHZ_HP  (250.0)
#define MCU_COMPUTE_POWER_MW_HP   (MCU_COMPUTE_UW_PER_MHZ_HP * MCU_COMPUTE_CLOCK_MHZ_HP / 1000.0) // 11.700 mW

/* Inference power for this demo's model set, one figure per operating point.
 * Source: OneDrive .../benchmarks/apollo510_evb/{ecg_segmentation,
 *         ecg_arrhythmia}/runlog.csv, LP(mW) and HP(mW) columns, row 2, dated
 *         2026-02-26.
 * Measured LP: 5.496 mW (ecg_segmentation), 5.743 mW (ecg_arrhythmia).
 * Measured HP: 16.697 mW (ecg_segmentation), 17.698 mW (ecg_arrhythmia).
 * Selection rule, identical in both sets: take the segmentation value rounded,
 *         i.e. the LOWER of the two, because segmentation is the stage that
 *         runs most often. ecg_denoise power was not captured in that run
 *         (all-zero row) in either column, so denoise -- the most frequently
 *         executed stage -- is billed at the segmentation figure. See the
 *         optimism note in the header above; both of these are choices that
 *         push the reported battery life up, not down.
 * Conditions: apollo510_evb, TFLM, AS R5.3.0, gcc 14.3, EVB BOARD-LEVEL. */
#define MCU_INFERENCE_POWER_MW_LP (5.5)
#define MCU_INFERENCE_POWER_MW_HP (16.7)

/* 20% system margin (divide by 0.80). STATED, NOT DERIVED. It covers what the
 * core figures above do not include: IOM / timer / GPIO activity driving the
 * sensor, and the 3.3 V to 1.8 V regulator loss (the datasheet figures are
 * quoted at VDD 1.8 V). It is an engineering allowance chosen by the owner, not
 * a measurement. It is scoped to THIS branch on purpose: it stands in for the
 * previous unexplained 0.77 factor, which only ever applied to the Apollo5
 * constants (issue #18). Applying it to the unsourced fallback branch below
 * would be inflating numbers that never carried it. */
#define SYSTEM_POWER_MARGIN (0.80)

#else

/* TODO(verify): 2.12 mW sleep power for apollo330mP_evb (AM_PART_APOLLO330P)
 * has no source of record (issue #18 searched OneDrive benchmarks, Confluence,
 * Jira, GitHub and git history and found none). Check the Apollo330P SoC
 * datasheet, "Current Consumption in Active Mode and Sleep Modes", for the
 * System Sleep 1 symbol, in OneDrive .../Ambiq/sws/datasheets/soc/. Value left
 * unchanged: replacing it with an Apollo510B number would be inventing
 * provenance, not fixing it. */
#define MCU_SLEEP_POWER_MW (2.12)

/* TODO(verify): 13.65 mW inference power for apollo330mP_evb has no source of
 * record (issue #18). Check the Apollo330P SoC datasheet for the CoreMark
 * run-power symbol, and bench the model set on this board the way
 * apollo510_evb was benched on 2026-02-26. Value left unchanged, and it is
 * used for both speed modes because no per-operating-point figure exists. */
#define MCU_INFERENCE_POWER_MW_LP (13.65)
#define MCU_INFERENCE_POWER_MW_HP (13.65)

/* TODO(verify): no general-compute figure exists for this part. Rather than
 * invent one, non-inference busy time is billed at the inference rate above --
 * i.e. this branch keeps the old, conservative two-state behaviour, and the
 * three-state split only bites where the figures are sourced. Replace with the
 * Apollo330P run-power symbol x the operating clock once verified. */
#define MCU_COMPUTE_POWER_MW_LP (MCU_INFERENCE_POWER_MW_LP)
#define MCU_COMPUTE_POWER_MW_HP (MCU_INFERENCE_POWER_MW_HP)

/* No margin on this branch. These values are unsourced and never carried the
 * previous 0.77 factor; applying the 20% system margin here would silently cut
 * the reported battery life on a board whose model has not otherwise changed.
 * When the figures above get a source, decide the margin with them. */
#define SYSTEM_POWER_MARGIN (1.0)

#endif

/* Battery capacity assumption -- 2 cells x 225 mAh x 3.3 V = 1485 mWh. This is
 * a BATTERY ASSUMPTION for the demo form factor, not a silicon claim and not a
 * measurement. Change it with the pack, and note that battery days scales
 * linearly with it. */
#define BATT_POWER_CAP (1485)

#define I2C_IOM (1)
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
#define AS7058_OSAL_INT_PIN 50
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

///////////////////////////////////////////////////////////////////////////////
// Observability (issue #11 -- see src/obs.h for the full argument)
///////////////////////////////////////////////////////////////////////////////
//
// There are exactly TWO flags here, and neither of them gates a counter.
//
// Counters and gauges are ALWAYS compiled in. Recording one is a relaxed
// load/add/store on a volatile uint32_t -- a few hundred per second across the
// whole app -- so there is no build in which switching them off buys anything
// worth the loss of evidence. What costs is PRINTING, and that is what these
// two flags control.
//
// This replaces three flags whose polarity was backwards: EN_APP_DEBUG_LOGS
// (deleted) defaulted to 0 and hid the cheap 1 Hz counter report, while
// EN_APP_TIMING_LOGS (deleted) defaulted to 1 and shipped the expensive
// per-pipeline-branch prints. The shipped build paid for the costly channel
// and omitted the useful one, and hardware validation of the emission rework
// was impossible until the flag was flipped by hand. EN_APP_EMIT_LOGS is
// folded into EN_APP_REPORT rather than surviving as a third flag.

/* Default 1: the periodic subsystem report emitted by ReportTask, one
 * subsystem per rotation slot at ~1 Hz per subsystem. This is where the
 * acceptance criteria for the streaming rework (packet rate, delivery, trim
 * rate, occupancy) are read from, so defaulting it off would leave every bench
 * run blind -- the mistake this whole flag scheme exists to correct.
 *
 * Setting it to 0 does NOT stop the counters; it only stops them being
 * printed. Use that for an A/B measurement of the reporter's own CPU cost, or
 * for a build where SWO is unavailable. */
#ifndef EN_APP_REPORT
#define EN_APP_REPORT (1)
#endif

/* Default 0: ad hoc per-event trace lines (HKV_TRACE_KV, src/obs.h).
 *
 * These sit inside the once-per-2-s denoise/segmentation/metrics branches on
 * the EQUAL-PRIORITY pump tasks whose cadence the latency budget below depends
 * on, and each line blocks its caller for the duration of the SWO write. A
 * blocked pump overruns its period, and an overrun is exactly the condition
 * tio_pump_wait() has to absorb without dropping below 1x. Enable deliberately
 * for a bring-up session; do not ship it on.
 *
 * Nothing is lost by leaving it off. Every value a trace line carried is also
 * counted (err_den/err_seg/err_met/err_ppgmet on the `pipe` report line), so a
 * persistently failing stage cannot hide behind a plausible-looking silence --
 * which is what made the old default dangerous rather than merely wasteful. */
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

/* TileIO TX queue depth, in packets. This is the buffer that absorbs host
 * jitter; the steady-state packet rate (signal slots plus the periodic metric
 * packets) and the seconds of hold it buys are worked out in #56.
 * A full queue (HKV_CNT_TIO_QDROP) is the largest but not the only loss path
 * with a connected host: see tio_tx_sm.h for the other two.
 * 48 x 256 B = 12 KiB of the FreeRTOS heap, sized in config/FreeRTOSConfig.h. */
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

///////////////////////////////////////////////////////////////////////////////
// TileIO latency budget
///////////////////////////////////////////////////////////////////////////////
//
// Design record: see issue #12 (streaming pipeline rework, sections 1 and
// 3.1-3.4 of the design record attached to it).
//
// A live monitor has two independent quantities, and they are not
// interchangeable:
//
//   D = fixed pipeline delay (sensor -> render). Dominated by the denoise and
//       segmentation windows. Invisible on a scrolling waveform.
//   J = arrival jitter, i.e. the spread of inter-packet spacing at the host.
//       This is what the host's playout buffer has to absorb and what its
//       staleness rule punishes.
//
// The pre-fix code minimised D and left J unbounded: ECG TX samples were only
// produced inside the segmentation branch, 200 samples (2 s at 100 Hz) at a
// time, and drained at up to 40 samples per 100 ms tick. The host saw 2 s of
// signal inside ~500 ms followed by ~1.5 s of nothing, overran its 1500 ms
// retention, and rendered a gap every 2 s in completely normal operation.
//
// The constants below trade D (which nobody can see) for a hard bound on J
// (which everybody can see).

/* Worst-case inter-packet spacing permitted at the host, per signal slot.
 * 250 ms = 50% of the host's 500 ms playout delay (2x margin) and 17% of its
 * 1500 ms staleness threshold. Exceeding it is a user-visible gap. */
#define TIO_JITTER_BUDGET_MS (250)

/* INVARIANT: TIO_MAX_EMIT_RATE = 1.00x realtime -- never exceed.
 *
 * After a stall of T seconds the firmware holds T extra seconds of signal.
 * Draining that backlog at *any* rate above 1x necessarily adds T to the
 * host's playout latency permanently, leaving it T closer to its staleness
 * threshold forever; there is no catch-up rate at which this works. The only
 * correct policy for a live monitor is to discard the stale backlog at the
 * source (trim to high-water, below) and resume at 1x. One honest gap of
 * exactly T renders, and latency returns to nominal immediately.
 *
 * WHAT ACTUALLY ENFORCES THIS: the pump pops from a ring, so it can only ever
 * emit samples the producer has already produced. Averaged over any window
 * longer than the buffer, emitted rate <= produced rate is guaranteed by the
 * ring itself, whatever count the pump chooses to pop. The invariant is
 * structural, not a property of the packet size.
 *
 * That distinction matters because a rigidly fixed pop count does NOT protect
 * the invariant -- it only fails in the other direction. Hardware run
 * 2026-09-01 measured the PPG producer ~1.1% faster than a fixed
 * 10-samples-per-100-ms pump: occupancy pinned at H and the trim discarded
 * ~1.1 samples/s permanently, forever, in a perfectly healthy system. A second
 * capture found ECG doing the same at ~1.8 samples/s and ~2.4% fast.
 *
 * So the pump tracks the producer instead of assuming it: a trough servo
 * estimates the standing rate error and spends a bounded budget of extra
 * samples to cancel it, and below the packet size the pump emits nothing at
 * all. Both corrections are bounded by what the ring holds, so neither can
 * push the average above the producer's true rate.
 *
 * The cost of the skip is jitter, not loss: no sample is discarded, the next
 * packet simply arrives one pump interval later. That is absorbed by the
 * host's 500 ms playout buffer and produces no waveform discontinuity, which
 * is the trade this whole file exists to make -- permanent loss is what is
 * worth avoiding, a bounded arrival delay is not. */

/* Pump period for the ECG and PPG signal slots. At configTICK_RATE_HZ = 1000
 * this is exactly 100 ticks, so pdMS_TO_TICKS() is exact and the pump can be
 * paced with vTaskDelayUntil() without rounding drift. */
#define TIO_PUMP_INTERVAL_MS (100)

/* Phase offset applied to the PPG pump's initial wake time so the two signal
 * slots do not enqueue in the same tick. Both pump tasks are created
 * back-to-back at equal priority and would otherwise stay in lockstep
 * indefinitely, concentrating the packet budget into simultaneous bursts and
 * making the transport queue peakier than the average rate implies. A third of
 * the pump interval spreads the two slots evenly. */
#define TIO_PPG_PUMP_PHASE_MS (33)

/* Samples per signal packet. 10 samples at ECG_TARGET_RATE/PPG_TARGET_RATE =
 * 100 Hz is exactly TIO_PUMP_INTERVAL_MS of signal, which is what makes one
 * packet per pump tick equal to 1.00x realtime: 10 pkt/s per slot.
 *
 * Wire framing, deliberately: 10 samples x 6 B = 60 B of samples, ~72 B on the
 * wire once the TileIO slot header is added, inside a TIO_USB_PACKET_LEN
 * (256 B) frame -- about 72% padding, BY DESIGN. At ~24 pkt/s total (~6 kB/s)
 * on a full-speed bulk link, wire efficiency is not the scarce resource; the
 * jitter budget is. Packing more samples per packet directly widens
 * inter-packet spacing and spends the very budget this file exists to protect.
 * Do not "optimise" the padding away. */
#define TIO_ECG_SAMPLES_PER_PKT (10)
#define TIO_PPG_SAMPLES_PER_PKT (10)

/* Extra samples the pump may take in a single tick when the trough servo has
 * budget to spend. Deliberately ONE: the correction needed is a couple of
 * samples per second against 100 produced, so it only has to be applied on a
 * few ticks per second, and a larger per-tick step would make each correction
 * a visible rate excursion rather than a nudge. The servo controls HOW MANY
 * ticks take the extra; this controls how much each one takes.
 *
 * Packets are therefore 10 or 11 samples, i.e. 60 or 66 B of payload. The
 * TimedSignal header carries the length and the host reads dlen, so variable
 * packet size is fine on the wire. */
#define TIO_TX_DRIFT_CATCHUP_SAMPLES (1)

/* TARGET TROUGH: the TX occupancy the pump aims to sit at between producer
 * blocks. This is the servo's reference, not a trigger threshold.
 *
 * Two packets = 200 ms of buffered signal. A full packet must always be in
 * hand at the trough or the pump skips a tick and opens an emission gap; the
 * second packet is margin for scheduling jitter and for a block arriving late.
 *
 * Bounded above, and the real bound is NOT simply troughTarget + block <= H.
 * Four things sit between the target and the peak the trim actually sees:
 * the trough lands up to PULL_DIV above target (the position term's
 * deadband); it alternates a further ~samplesPerPkt from the block-period
 * quantisation described at TIO_TX_SERVO_WINDOW_TICKS; and the block itself
 * can arrive up to TIO_TX_SLIP_SAMPLES late, landing on a trough the servo
 * has meanwhile raised. So the bound is
 *
 *     troughTarget + PULL_DIV + samplesPerPkt + slip + block <= H
 *
 * ECG 20 + 8 + 10 + 30 + 200 = 268 <= 280; PPG 20 + 8 + 10 + 30 + 13 = 81
 * <= 93. That is what main.cc asserts.
 *
 * This bound has been wrong twice, both times by omitting a term, so add
 * rather than simplify. The naive two-term form passed at a target of 30
 * while the real peak sat at 248 of 250; the four-term form that replaced it
 * still ignored producer scheduling and was over-optimistic by an order of
 * magnitude in margin. The five-term value above matches simulation exactly:
 * at 102.4 samples/s with three iterations of slip the simulated peak is 268. */
#define TIO_ECG_TX_TROUGH_TARGET (2 * TIO_ECG_SAMPLES_PER_PKT)
#define TIO_PPG_TX_TROUGH_TARGET (2 * TIO_PPG_SAMPLES_PER_PKT)

/* Servo window, in pump ticks. 64 ticks = 6.4 s.
 *
 * Sized to span SEVERAL producer blocks, not merely one, because of an
 * aliasing effect measured on hardware 2026-09-01. The ECG block period is
 * 200 samples at the producer's true ~102.4 samples/s = 1.953 s = 19.53 pump
 * ticks. Being a non-integer number of ticks, consecutive blocks drain over
 * alternately 19 or 20 ticks, so the true trough alternates by a full
 * samplesPerPkt (10) from block to block. That is quantisation beat between
 * the 100 ms pump grid and the block period -- not drift, and not something
 * the servo should react to.
 *
 * With the earlier 32-tick window (1.64 blocks) some windows caught one trough
 * and some caught two, so the measured minimum alternated by that same 10 and
 * the servo's rate term differentiated the artifact: measured trough
 * alternating 13<->23 and budget slamming 0<->10 every window.
 *
 * HONEST RESULT: widening to 64 ticks did NOT fix that. Measured after the
 * change, the budget spread WIDENED to 0..31 and the trough to 3..34. The
 * aliasing analysis above is sound as far as it goes, but it was not the whole
 * cause, and 64 ticks is 3.28 block periods -- still not an integer multiple,
 * so the count and phase of the troughs captured per window keep changing. A
 * fixed-length window cannot be an integer multiple of a block period set by
 * an independent, drifting producer clock. See the RESIDUAL BEHAVIOUR block at
 * tio_tx_group_servo() in main.cc for the measured distributions, why it is
 * shipped anyway, and what to investigate instead. Do not assume this constant
 * solved the dither.
 *
 * The window is nonetheless kept at 64 rather than reverted to 32, because the
 * larger window came with the corrected four-term peak bound and both variants
 * meet acceptance identically -- safety margin over a tighter-looking counter.
 *
 * The cost is responsiveness, but less than a per-step view suggests: the rate
 * term is dead-beat, so simulation reaches a working budget on the SECOND
 * window (~12.7 s from boot), not over the many windows a one-step-per-window
 * reading would imply. An earlier version of this comment claimed ~50 s and
 * contradicted the dead-beat description of the control law in main.cc; 12.7 s
 * is the simulated figure. The residual dither never settles at all, so there
 * is no later "converged" point to wait for. */
#define TIO_TX_SERVO_WINDOW_TICKS (64)

/* Divisor on the servo's position term: each window it moves the budget by
 * (trough - target) / this. It is the gentle term -- the rate term does the
 * actual drift cancellation -- so it is deliberately weak, giving a
 * first-order approach to target over roughly this many windows.
 *
 * Being an integer division it also supplies a deadband: while the trough is
 * within +/-8 samples (80 ms) of target this contributes exactly zero and the
 * servo holds still rather than dithering. The deadband is why the trough
 * settles somewhat ABOVE target rather than on it, which the peak bound at
 * TIO_*_TX_TROUGH_TARGET accounts for explicitly. Lowering it would tighten
 * that offset at the cost of overshoot risk. */
#define TIO_TX_SERVO_PULL_DIV (8)

/* Budget ceiling, in extra samples per window. 32 per 6.4 s = 5 samples/s,
 * i.e. headroom for ~5% producer drift against the ~2.4% measured -- scaled
 * with the window so the ceiling stays a rate, not a count. Also the
 * anti-windup clamp: a stalled producer parks the budget here-or-zero rather
 * than accumulating a debt it would later spend as a burst. Reaching this
 * ceiling in steady state means drift exceeds what the servo can correct and
 * the trim will start absorbing the remainder.
 *
 * MEASURED 2026-09-01: the budget was observed at 31 -- one below this cap --
 * on 13 lines of a 107 s settled ECG window, at ~2.4% producer drift. It is
 * not pinned there, but it does reach it intermittently as part of the dither
 * documented at tio_tx_group_servo(). If you find it sitting AT the cap, know
 * that it was already touching 31 at normal drift, so the cap is probably not
 * the problem; and note that this clamp is what keeps the budget from becoming
 * a burst, so raising it is not automatically the right response. */
#define TIO_TX_SERVO_MAX_BUDGET (32)

/* Largest packet either signal slot can emit -- sizes the sender stack buffers
 * and must account for the drift drain above, not just the nominal size. */
#define TIO_ECG_MAX_SAMPLES_PER_PKT (TIO_ECG_SAMPLES_PER_PKT + TIO_TX_DRIFT_CATCHUP_SAMPLES)
#define TIO_PPG_MAX_SAMPLES_PER_PKT (TIO_PPG_SAMPLES_PER_PKT + TIO_TX_DRIFT_CATCHUP_SAMPLES)

/* Per-slot TX-ring high-water H, in samples. On each pump tick the slot's TX
 * rings are trimmed to H before popping, so H bounds the stale backlog the
 * firmware is willing to hold. H is a sum of four terms:
 *
 *   1. structural block  -- the largest burst the producer can deliver in one
 *                           go, which the consumer must be able to hold
 *                           without discarding anything in steady state;
 *   2. one packet        -- TIO_*_SAMPLES_PER_PKT, so a full packet can always
 *                           be assembled from what is left after a trim;
 *   3. scheduling slack  -- TIO_TX_SLACK_SAMPLES, for a late PUMP tick;
 *   4. block slip        -- TIO_TX_SLIP_SAMPLES, for a late PRODUCER block.
 *                           Terms 3 and 4 are different failure modes and both
 *                           are needed: a late pump leaves samples unread, a
 *                           late block lands them all at once on top of a
 *                           trough the servo has already raised.
 *
 * Steady-state trim should be ~0 in BOTH clock-drift directions, and that is
 * the acceptance criterion. The pump's nominal rate is anchored to the
 * FreeRTOS tick while the producer is anchored to the AS7058 sample clock, and
 * nothing cross-checks the two, so the pump corrects for the difference at
 * both ends rather than assuming it away:
 *
 *   * Producer slightly SLOW: the trough falls, the servo winds its budget
 *     down to 0, and the pump self-throttles by skipping a tick whenever it
 *     holds less than a full packet. No loss; samples cannot be manufactured.
 *   * Producer slightly FAST: the trough rises, the servo winds its budget up
 *     and spends extra samples on a few ticks per second until the trough
 *     returns to target, so occupancy never approaches H. No loss.
 *
 * Both directions were observed as permanent loss before the servo existed:
 * hardware measured PPG trimming ~1.1 samples/s and ECG ~1.8 samples/s
 * indefinitely, each with a threshold drain that could not be tuned to fix one
 * without breaking the other.
 *
 * TIO_TX_TRIM_DRIFT_ALLOWANCE_SPS is retained only as a backstop for judging a
 * capture, not as an expectation: sustained trim at any appreciable rate now
 * means the servo is saturated (drift beyond TIO_TX_SERVO_MAX_BUDGET), H is
 * mis-derived, or the producer is genuinely misbehaving. Any of those is a bug
 * to investigate, not a policy working as intended. */

/* Backstop tolerance when judging a bench capture, in samples/s. Expect 0.
 * 0.5% of a 100 Hz stream is 0.5 samples/s; round up to 1. */
#define TIO_TX_TRIM_DRIFT_ALLOWANCE_SPS (1)

/* Scheduling slack for a late PUMP tick: TIO_JITTER_BUDGET_MS (250 ms) plus
 * one pump interval (100 ms) = 350 ms, rounded up to the next whole pump
 * interval = 400 ms. At 100 Hz that is 40 samples. */
#define TIO_TX_SLACK_SAMPLES (40)

/* Block slip: allowance for a late PRODUCER block, in samples. Three pump
 * intervals = 300 ms = 30 samples at 100 Hz.
 *
 * WHY THIS EXISTS. EcgProcessTask runs exactly ONE mutually-exclusive pipeline
 * stage per iteration, and the 200-sample push happens only in the
 * segmentation branch. A due segmentation is therefore deferrable by whole
 * iterations whenever a higher-priority branch claims the tick -- denoise is
 * checked first, so it can push segmentation out by an iteration. The block
 * then lands late, all at once, on top of a trough the servo has already
 * raised toward target. Slack (term 3) does not cover this: it is sized for
 * the opposite failure, a pump that is late reading.
 *
 * WHY THREE ITERATIONS. One is the steady-state maximum, since denoise is the
 * only higher-priority branch and it fires once per block cycle. Three also
 * covers a denoise inference overrunning its 100 ms budget, and the transient
 * multi-iteration deferral possible after a flush or reconnect when rbEcgDen
 * carries a backlog. It matches the 0-300 ms slip envelope the change was
 * reviewed against.
 *
 * MEASURED CONSEQUENCE OF OMITTING IT. Simulated at the measured 102.4
 * samples/s with the previous H of 250: one iteration of slip put peak
 * pre-trim occupancy at 247 against H = 250 -- three samples of margin, not
 * the ~41 that had been documented -- and two iterations reached 255 and
 * discarded samples, reinstating the permanent-loss defect this branch
 * exists to remove. With the slip term included, the same sweep peaks at 268
 * against H = 280 and trims nothing. */
#define TIO_TX_SLIP_SAMPLES (30)

/* ECG structural block: the segmentation branch is the sole producer of the
 * ECG TX taps and it pushes one whole ECG_SEG_VALID_LEN batch per cycle
 * (main.cc, ECG SEGMENTATION). See #36. */
#define TIO_ECG_TX_BLOCK_SAMPLES (ECG_SEG_VALID_LEN)
#define TIO_ECG_TX_HIGH_WATER                                                                                          \
    (TIO_ECG_TX_BLOCK_SAMPLES + TIO_ECG_SAMPLES_PER_PKT + TIO_TX_SLACK_SAMPLES + TIO_TX_SLIP_SAMPLES)

/* PPG structural block: PpgProcessTask tees samples continuously, but the
 * AS7058 delivers on a FIFO watermark whose observed ISR interval is
 * ~125-130 ms, so one pump tick can find up to ~13 samples at 100 Hz.
 * 13 + 10 + 40 + 30 = 93.
 *
 * PPG takes the same slip allowance as ECG, for a different reason. Its
 * producer is not a deferrable stage -- the tee loop runs every iteration --
 * but a delayed PpgProcessTask lets the sensor ring accumulate, so the slip
 * shows up as a LARGER TEE BURST rather than a later block. The allowance
 * covers a 300 ms task delay, which is the same envelope. Costs nothing:
 * measured PPG occupancy runs 0..22 against the old H of 63, so it never
 * approached either bound, and the ring headroom is enormous either way.
 *
 * CAVEAT, and it is a weaker guarantee than the ECG line above: 13 is an
 * OBSERVED figure, not a compile-time bound. The ECG block is
 * ECG_SEG_VALID_LEN, a constant the static_asserts in main.cc can check. The
 * PPG tee loop, with PPG_DS_RATE == 1, is bounded only by the sensor ring
 * occupancy MIN(len(rbPpg1Sensor), len(rbPpg2Sensor)) -- up to
 * SENSOR_BUF_LEN-1 (255) if that task is ever delayed long enough. Exceeding
 * this constant does not corrupt anything (the trim absorbs it) but it does
 * mean H is under-derived and steady-state trim would become non-zero. The
 * HKV_GAUGE_PPG_TEE gauge (src/obs.h) exists to catch that on the bench: it is
 * declared LIFETIME rather than windowed precisely so a single excursion
 * cannot scroll out of the capture. Read it as `tee_hi` on the `pipe` report
 * line; if it exceeds this value, re-derive H. */
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
