/**
 * @file main.c
 * @author Adam Page (adam.page@ambiq.com)
 * @brief HeartKit demo
 * @version 1.0
 * @date 2024-04-16
 *
 * @copyright Copyright (c) 2024
 *
 */

// neuralSPOT
#include "ns_ambiqsuite_harness.h"
#include "ns_peripherals_power.h"
#include "FreeRTOS.h"
#include "task.h"
#include "arm_math.h"
#include "ns_i2c.h"
#include "ns_spi.h"
#include "webusb_controller.h"
// Modules
#include "pk_filter.h"
#include "pk_ecg.h"
#include "pk_math.h"
#include "tio_usb.h"
#include "ledstick.h"
// Locals
#include "main.h"
#include "constants.h"
#include "store.h"
#include "pmic.h"

#include "sensor.h"
#include "nstdb_noise.h"
#include "tflm.h"
#include "ecg_segmentation.h"
#include "ecg_denoise.h"
#include "ecg_arrhythmia.h"
#include "metrics.h"
#include "ringbuffer.h"

#if APOLLO_SOC_TYPE == APOLLO5_SOC
#include <arm_mve.h>
#endif

#define HEAP_SIZE (1024)
#if (configAPPLICATION_ALLOCATED_HEAP == 1)
    #define APP_HEAP_SIZE (8 * 4 * 1024)
size_t ucHeapSize = APP_HEAP_SIZE;
uint8_t ucHeap[APP_HEAP_SIZE] __attribute__((aligned(4)));
#endif

void send_uio_state(void);

volatile uint32_t g_rtos_stat_timer_cnt = 0;

uint32_t
rtos_time_init() {
    uint32_t timerNum = RTOS_TIMER;
    uint32_t ui32Status;
    g_rtos_stat_timer_cnt = 0;
    am_hal_timer_config_t rtosTimerConfig;
    am_hal_timer_default_config_set(&rtosTimerConfig);
    // 4096/(96 megahertz)*6 = 256us
    rtosTimerConfig.eInputClock = AM_HAL_TIMER_CLOCK_HFRC_DIV4K;
    rtosTimerConfig.eTriggerSource = AM_HAL_TIMER_TRIGGER_TMR4_OUT1;
    ui32Status = am_hal_timer_config(timerNum, &rtosTimerConfig);
    am_hal_timer_clear(timerNum);
    return ui32Status;
}

uint32_t
rtos_ticker_read() {
    uint32_t timerNum = RTOS_TIMER;
    uint32_t val;
    val = am_hal_timer_read(timerNum);
    return val;
}

uint32_t
rtos_timer_clear() {
    uint32_t timerNum = RTOS_TIMER;
    am_hal_timer_clear(timerNum);
    return 0;
}

void RTOS_AppConfigureTimerForRuntimeStats(void) {
    g_rtos_stat_timer_cnt = 0;
}

uint32_t RTOS_AppGetRuntimeCounterValueFromISR(void) {
    // Check for overflow
    if (g_rtos_stat_timer_cnt > 0x7FFFFFFF) {
        g_rtos_stat_timer_cnt = 0;
        rtos_timer_clear();
    }
    g_rtos_stat_timer_cnt = rtos_ticker_read();
    return g_rtos_stat_timer_cnt;
}

static TaskHandle_t ecgTaskHandle;
static TaskHandle_t ppgTaskHandle;
static TaskHandle_t cpuTaskHandle;
static TaskHandle_t appSetupTask;
static TaskStatus_t xTaskDetails[5];
static
tio_usb_context_t tioUsbCtx = {
    .uio_update_cb = NULL,
    .slot_update_cb = NULL
};

///////////////////////////////////////////////////////////////////////////////
// APP STATE
///////////////////////////////////////////////////////////////////////////////

/**
 * @brief Flush pipeline buffers
 *
 */
void flush_pipeline() {
    ringbuffer_flush(&rbEcgSensor);
    ringbuffer_flush(&rbEcgDen);
    ringbuffer_flush(&rbEcgRawSeg);
    ringbuffer_flush(&rbEcgSeg);
    ringbuffer_flush(&rbEcgMet);
    ringbuffer_flush(&rbEcgMaskMet);
    ringbuffer_flush(&rbEcgRawTx);
    ringbuffer_flush(&rbEcgDenTx);
    ringbuffer_flush(&rbEcgMaskTx);
    ringbuffer_flush(&rbPpg1Sensor);
    ringbuffer_flush(&rbPpg2Sensor);
    ringbuffer_flush(&rbPpg1Seg);
    ringbuffer_flush(&rbPpg2Seg);
    ringbuffer_flush(&rbPpg1Met);
    ringbuffer_flush(&rbPpg2Met);
    ringbuffer_flush(&rbPpg1Tx);
    ringbuffer_flush(&rbPpg2Tx);
}

static uint32_t g_webusb_available = false;
void
check_webusb_state() {
    uint32_t webusb_available = webusb_is_connected();
    if (webusb_available != g_webusb_available) {
        g_webusb_available = webusb_available;
        if (g_webusb_available) {
            send_uio_state();
        }
    }
}

void
set_input_source(uint8_t source) {
    source = MIN(source, NUM_INPUT_PTS);
    if (appState.inputSource != source) {
        appState.inputSource = source;
        sensorCtx.inputSource = source;
        ns_lp_printf("Input Source: %d\n", sensorCtx.inputSource);
        // flush_pipeline();
    }
}

void
set_noise_inputs(uint8_t bw, uint8_t ma, uint8_t em) {
    appState.bwNoiseLevel = bw;
    appState.maNoiseLevel = ma;
    appState.emNoiseLevel = em;
    ns_lp_printf("Noise Level: %d,%d,%d\n", appState.bwNoiseLevel, appState.maNoiseLevel, appState.emNoiseLevel);
}

void
set_denoise_mode(uint8_t mode) {
    mode = MIN(mode, 2);
    if (appState.denoiseMode != mode) {
        appState.denoiseMode = mode;
        ns_lp_printf("Denoise Mode: %d\n", appState.denoiseMode);
    }
}

void
set_segmentation_mode(uint8_t mode) {
    mode = MIN(mode, 2);
    if (appState.segMode != mode) {
        appState.segMode = mode;
        ns_lp_printf("Segmentation Mode: %d\n", appState.segMode);
    }
}

void
set_arrhythmia_mode(uint8_t mode) {
    mode = MIN(mode, 2);
    if (appState.arrMode != mode) {
        appState.arrMode = mode;
        ns_lp_printf("Arrhythmia Mode: %d\n", appState.arrMode);
    }
}

void
set_speed_mode(uint8_t mode) {
    mode = MIN(mode, 1);
    if (appState.speedMode != mode) {
        appState.speedMode = mode;
        ns_set_performance_mode(mode ? NS_MAXIMUM_PERF : NS_MINIMUM_PERF);
        ns_lp_printf("CPU Speed Mode: %d\n", appState.speedMode);
    }
}


///////////////////////////////////////////////////////////////////////////////
// TIO
///////////////////////////////////////////////////////////////////////////////


/**
 * @brief Send ECG (slot0) signals to TIO
 *
 */
void
send_ecg_signals() {
    uint8_t buffer[240];
    float32_t val_f32;
    uint16_t val_u16;
    int16_t val_i16;
    uint32_t length;
    size_t numSamples = MIN3(
        ringbuffer_len(&rbEcgRawTx),
        ringbuffer_len(&rbEcgDenTx),
        ringbuffer_len(&rbEcgMaskTx)
    );
    if (numSamples == 0) { return; }
    numSamples = MIN(numSamples, 240/(3*sizeof(int16_t)));
    length = 0;
    for (size_t i = 0; i < numSamples; i++) {
        ringbuffer_pop(&rbEcgMaskTx, &val_u16, 1);
        memcpy(&buffer[length], &val_u16, sizeof(uint16_t));
        length += sizeof(uint16_t);
        ringbuffer_pop(&rbEcgRawTx, &val_f32, 1);
        val_i16 = (int16_t)CLIP(TIO_SLOT0_SCALE*val_f32, -32768, 32767);
        memcpy(&buffer[length], &val_i16, sizeof(int16_t));
        length += sizeof(int16_t);
        ringbuffer_pop(&rbEcgDenTx, &val_f32, 1);
        val_i16 = (int16_t)CLIP(TIO_SLOT0_SCALE*val_f32, -32768, 32767);
        memcpy(&buffer[length], &val_i16, sizeof(int16_t));
        length += sizeof(int16_t);
    }
    tio_usb_send_slot_data(0, 0, (uint8_t *)buffer, length);
}

/**
 * @brief Send ECG (slot0) metrics to TIO
 *
 */
void
send_ecg_metrics() {
    float32_t buffer[60];
    buffer[0] = ecgMetResults.hr;
    buffer[1] = ecgMetResults.hrv;
    buffer[2] = ecgMetResults.denoiseCossim;
    buffer[3] = ecgMetResults.arrhythmiaLabel;
    buffer[4] = ecgMetResults.denoiseIps;
    buffer[5] = ecgMetResults.segmentIps;
    buffer[6] = ecgMetResults.arrhythmiaIps;
    buffer[7] = ecgMetResults.qos;
    tio_usb_send_slot_data(0, 1, (uint8_t *)buffer, 8*sizeof(float32_t));
}


/**
 * @brief Send PPG (slot1) signals to TIO
 *
 */
void
send_ppg_signals() {
    uint8_t buffer[240];
    float32_t val_f32;
    int16_t val_i16;
    uint32_t length;
    uint8_t qos = ppgMetResults.qos / 25;
    uint16_t mask = (qos << SIG_MASK_QOS_OFFSET);
    size_t numSamples = MIN(
        ringbuffer_len(&rbPpg1Tx),
        ringbuffer_len(&rbPpg2Tx)
    );
    numSamples = MIN(numSamples, 240/(1*sizeof(int16_t) + 2*sizeof(int16_t)));
    if (numSamples == 0) { return; }
    length = 0;
    for (size_t i = 0; i < numSamples; i++) {
        memcpy(&buffer[length], &mask, sizeof(uint16_t));
        length += sizeof(uint16_t);
        ringbuffer_pop(&rbPpg1Tx, &val_f32, 1);
        // val_i16 = (int16_t)CLIP(TIO_SLOT0_SCALE*val_f32, -32768, 32767);
        val_i16 = val_f32;
        memcpy(&buffer[length], &val_i16, sizeof(int16_t));
        length += sizeof(int16_t);
        ringbuffer_pop(&rbPpg2Tx, &val_f32, 1);
        // val_i16 = (int16_t)CLIP(TIO_SLOT0_SCALE*val_f32, -32768, 32767);
        val_i16 = val_f32;
        memcpy(&buffer[length], &val_i16, sizeof(int16_t));
        length += sizeof(int16_t);
    }
    tio_usb_send_slot_data(1, 0, (uint8_t *)buffer, length);
}

/**
 * @brief Send PPG (slot1) metrics to TIO
 *
 */
void
send_ppg_metrics() {
    float32_t buffer[60];
    buffer[0] = ppgMetResults.pr;
    buffer[1] = ppgMetResults.spo2;
    buffer[2] = ppgMetResults.qos;
    tio_usb_send_slot_data(1, 1, (uint8_t *)buffer, 3*sizeof(float32_t));
}

void
send_cpu_signals() {
    uint8_t buffer[240];
    float32_t val_f32;
    uint32_t length;
    uint8_t qos = SIG_QOS_GOOD;
    uint16_t mask = (qos << SIG_MASK_QOS_OFFSET);
    size_t numSamples = MIN3(
        ringbuffer_len(&rbEcgCpuTx),
        ringbuffer_len(&rbPpgCpuTx),
        ringbuffer_len(&rbTotalCpuTx)
    );
    if (numSamples == 0) { return; }
    numSamples = MIN(numSamples, 240/(1*sizeof(int16_t) + 3*sizeof(float32_t)));
    length = 0;
    for (size_t i = 0; i < numSamples; i++) {
        memcpy(&buffer[length], &mask, sizeof(uint16_t));
        length += sizeof(uint16_t);
        ringbuffer_pop(&rbEcgCpuTx, &val_f32, 1);
        memcpy(&buffer[length], &val_f32, sizeof(float32_t));
        length += sizeof(float32_t);
        ringbuffer_pop(&rbPpgCpuTx, &val_f32, 1);
        memcpy(&buffer[length], &val_f32, sizeof(float32_t));
        length += sizeof(float32_t);
        ringbuffer_pop(&rbTotalCpuTx, &val_f32, 1);
        memcpy(&buffer[length], &val_f32, sizeof(float32_t));
        length += sizeof(float32_t);
    }
    tio_usb_send_slot_data(2, 0, (uint8_t *)buffer, length);
}

/**
 * @brief Send CPU (slot2) metrics to TIO
 *
 */
void
send_cpu_metrics() {
    float32_t buffer[60];
    buffer[0] = appMetResults.cpuPercUtil;
    buffer[1] = appMetResults.batteryHours;
    tio_usb_send_slot_data(2, 1, (uint8_t *)buffer, 2*sizeof(float32_t));
}

void
send_uio_state() {
    uint8_t uioBuffer[8];
    uioBuffer[TIO_UIO_INPUT_SEL_IDX] = appState.inputSource;
    uioBuffer[TIO_UIO_BW_NOISE_IDX] = appState.bwNoiseLevel;
    uioBuffer[TIO_UIO_MA_NOISE_IDX] = appState.maNoiseLevel;
    uioBuffer[TIO_UIO_EM_NOISE_IDX] = appState.emNoiseLevel;
    uioBuffer[TIO_UIO_SPEED_MODE_IDX] = appState.speedMode;
    uioBuffer[TIO_UIO_DEN_MODE_IDX] = appState.denoiseMode;
    uioBuffer[TIO_UIO_SEG_MODE_IDX] = appState.segMode;
    uioBuffer[TIO_UIO_ARR_MODE_IDX] = appState.arrMode;
    tio_usb_send_uio_state(uioBuffer, 8);
}

void
received_slot_data(uint8_t slot, uint8_t slot_type, const uint8_t *data, uint32_t length) {
    // No slot data expected
}

void
received_uio_state(const uint8_t *data, uint32_t length) {
    set_input_source(data[TIO_UIO_INPUT_SEL_IDX]);
    set_noise_inputs(
        data[TIO_UIO_BW_NOISE_IDX],
        data[TIO_UIO_MA_NOISE_IDX],
        data[TIO_UIO_EM_NOISE_IDX]
    );
    set_speed_mode(data[TIO_UIO_SPEED_MODE_IDX]);
    set_denoise_mode(data[TIO_UIO_DEN_MODE_IDX]);
    set_segmentation_mode(data[TIO_UIO_SEG_MODE_IDX]);
    set_arrhythmia_mode(data[TIO_UIO_ARR_MODE_IDX]);
    send_uio_state();
}


void
CpuProcessTask(void *pvParameters)
{
    uint32_t cpuIdlePerc = 0;
    uint32_t runTimeTicks = 0;
    float32_t ecgTaskPerc = 0, ppgTaskPerc = 0, totalTaskPerc = 0;
    uint32_t prevRun = 0, prevEcg = 0, prevPpg = 0;
    uint32_t runDelta, ecgDelta, ppgDelta;
    size_t numTasks = 0, counter = 0;
    while (true) {
        cpuIdlePerc = ulTaskGetIdleRunTimePercent();
        numTasks = uxTaskGetSystemState(xTaskDetails, 5, &runTimeTicks);
        runDelta = runTimeTicks - prevRun;
        prevRun = runTimeTicks;
        for (size_t i = 0; i < numTasks; i++) {
            // Get utilization for ECG task
            if (xTaskDetails[i].xTaskNumber == 1)
            {
                ecgDelta = xTaskDetails[i].ulRunTimeCounter - prevEcg;
                prevEcg = xTaskDetails[i].ulRunTimeCounter;
                ecgTaskPerc = 100.0f*(float32_t)ecgDelta/(float32_t)runDelta;
            }
            else if (xTaskDetails[i].xTaskNumber == 2)
            {
                ppgDelta = xTaskDetails[i].ulRunTimeCounter - prevPpg;
                prevPpg = xTaskDetails[i].ulRunTimeCounter;
                ppgTaskPerc = 100.0f*(float32_t)ppgDelta/(float32_t)runDelta;
            }
        }

        // NOTE: Cant read live PMIC since sensor I/O leaks current to MCU voltage rail
        // pmic_read_values(&g_pmicMetrics);
        // pmic_display_values(&g_pmicMetrics);
        totalTaskPerc = ecgTaskPerc + ppgTaskPerc;
        appMetResults.cpuPercUtil = 100 - cpuIdlePerc;
        float32_t avgPower = ((100 - cpuIdlePerc)*8.90 + cpuIdlePerc*1.50)/100.0;
        // CR2032 225 mah battery x 3.3V = 742.5 mWh
        appMetResults.batteryHours = (225*3.3)/avgPower;

        ringbuffer_push(&rbEcgCpuTx, &ecgTaskPerc, 1);
        ringbuffer_push(&rbPpgCpuTx, &ppgTaskPerc, 1);
        ringbuffer_push(&rbTotalCpuTx, &totalTaskPerc, 1);

        send_cpu_signals();

        // Every 1 second
        if (counter % 10 == 0) {
            send_cpu_metrics();
        }
        counter += 1;

        // Delay 100 ms (10 Hz)
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


///////////////////////////////////////////////////////////////////////////////
// ECG PROCESS TASK
///////////////////////////////////////////////////////////////////////////////

void
EcgProcessTask(void *pvParameters) {
    uint32_t err = 0;
    uint32_t delayUs = 0, tickUs = 0;
    uint32_t deltaUs = 0;
    size_t numSamples = 0;

    while (true) {
        err = 0;
        ns_timer_clear(&ecgTimerCfg);
        check_webusb_state();

        ///////////////////////////////////////////////////////////////////////
        // ECG PREPROCESSING BLOCK
        ///////////////////////////////////////////////////////////////////////
        numSamples = ringbuffer_len(&rbEcgSensor);
        for (size_t i = 0; i < numSamples/ECG_DS_RATE; i++) {
            ringbuffer_seek(&rbEcgSensor, ECG_DS_RATE - 1);
            ringbuffer_transfer(&rbEcgSensor, &rbEcgDen, 1);
        }

        ///////////////////////////////////////////////////////////////////////
        // ECG DENOSING BLOCK
        ///////////////////////////////////////////////////////////////////////
        if (ringbuffer_len(&rbEcgDen) >= ECG_DEN_WINDOW_LEN) {
            tickUs = ns_us_ticker_read(&ecgTimerCfg);

            // Grab data from ringbuffer
            ringbuffer_peek(&rbEcgDen, ecgDenInout, ECG_DEN_WINDOW_LEN);

            // Preprocess signal
            pk_standardize_f32(ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, NORM_STD_EPS);

            // Copy clean signal to buffer
            arm_copy_f32(ecgDenInout, ecgDenNoise, ECG_DEN_WINDOW_LEN);

            // Add noise to signal based on input
            if (sensorCtx.inputSource < NUM_INPUT_PTS) {
                nstdb_add_bw_noise(ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, (float32_t)appState.bwNoiseLevel*2.0e-5);
                nstdb_add_ma_noise(ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, (float32_t)appState.maNoiseLevel*1.0e-5);
                nstdb_add_em_noise(ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, (float32_t)appState.emNoiseLevel*1.0e-5);
            }

            // Copy noisy signal to seg buffer
            ringbuffer_push(&rbEcgRawSeg, &ecgDenInout[ECG_DEN_PAD_LEN], ECG_DEN_VALID_LEN);

            // Apply biquad filter for DSP and AI modes
            if (appState.denoiseMode == DenoiseModeDsp) {
                err = pk_apply_biquad_filtfilt_f32(&ecgFilterCtx, ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, ecgDenScratch);
            }
            // Denoise using AI model
            else if (appState.denoiseMode == DenoiseModeAi) {
                err = ecg_denoise_inference(ecgDenInout, ecgDenInout, 0, ECG_DEN_THRESHOLD);
            } else {
                err = 0;
            }

            // Compute cosine similarity
            if (sensorCtx.inputSource < NUM_INPUT_PTS) {
                cosine_similarity_f32(
                    &ecgDenInout[ECG_DEN_PAD_LEN],
                    &ecgDenNoise[ECG_DEN_PAD_LEN],
                    ECG_DEN_VALID_LEN,
                    &ecgMetResults.denoiseCossim
                );
            // Skip for live sensor mode
            } else {
                ecgMetResults.denoiseCossim = 1.0;
            }
            ecgMetResults.denoiseCossim *= 100.0;

            // Copy clean signal to seg buffer
            ringbuffer_push(&rbEcgSeg, &ecgDenInout[ECG_DEN_PAD_LEN], ECG_DEN_VALID_LEN);

            // Seek ringbuffers
            ringbuffer_seek(&rbEcgDen, ECG_DEN_VALID_LEN);
            deltaUs = ns_us_ticker_read(&ecgTimerCfg) - tickUs;
            ecgMetResults.denoiseIps = 1000000.0/deltaUs;
            ns_lp_printf("<ECG DENOISE Time: %d ms (err=%d) >\n", deltaUs/1000, err);
        }

        ///////////////////////////////////////////////////////////////////////
        // ECG SEGMENTATION BLOCK
        ///////////////////////////////////////////////////////////////////////
        else if (ringbuffer_len(&rbEcgSeg) >= ECG_SEG_WINDOW_LEN) {
            tickUs = ns_us_ticker_read(&ecgTimerCfg);
            ringbuffer_peek(&rbEcgSeg, ecgSegInout, ECG_SEG_WINDOW_LEN);

            // pk_standardize_f32(ecgSegInout, ecgSegInout, ECG_SEG_WINDOW_LEN, NORM_STD_EPS);

            if (appState.segMode == SegmentationModeDsp) {
                err = ecg_physiokit_segmentation_inference(ecgSegInout, ecgSegMask, 0, &ecgMetResults.qos);
            } else if (appState.segMode == SegmentationModeAi) {
                err = ecg_segmentation_inference(ecgSegInout, ecgSegMask, 0, ECG_SEG_THRESHOLD, &ecgMetResults.qos);
            } else{
                err = 0;
                for (size_t i = 0; i < ECG_SEG_WINDOW_LEN; i++) {
                    ecgSegMask[i] = ECG_SEG_NONE;
                }
            }

            // Push seg mask to Tx
            ringbuffer_transfer(&rbEcgRawSeg, &rbEcgRawTx, ECG_SEG_VALID_LEN);
            ringbuffer_push(&rbEcgDenTx, &ecgSegInout[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);
            ringbuffer_push(&rbEcgMaskTx, &ecgSegMask[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);

            // Push ecg and mask to metrics
            ringbuffer_push(&rbEcgMet, &ecgSegInout[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);
            ringbuffer_push(&rbEcgMaskMet, &ecgSegMask[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);

            ringbuffer_seek(&rbEcgSeg, ECG_SEG_VALID_LEN);
            deltaUs = ns_us_ticker_read(&ecgTimerCfg) - tickUs;
            ecgMetResults.segmentIps = 1000000.0/deltaUs;
            ns_lp_printf("<ECG SEGMENT Time: %d (err=%d) >\n", deltaUs/1000, err);
        }

        ///////////////////////////////////////////////////////////////////////
        // ECG ARRHYTHMIA/METRICS BLOCK
        ///////////////////////////////////////////////////////////////////////
        else if (MIN(ringbuffer_len(&rbEcgMet), ringbuffer_len(&rbEcgMaskMet)) >= ECG_MET_WINDOW_LEN) {
            tickUs = ns_us_ticker_read(&ecgTimerCfg);

            // Grab data from ringbuffers
            ringbuffer_peek(&rbEcgMet, ecgMetData, ECG_MET_WINDOW_LEN);
            ringbuffer_peek(&rbEcgMaskMet, ecgMaskMetData, ECG_MET_WINDOW_LEN);

            // Compute metrics
            err = metrics_capture_ecg(
                &metricsCfg,
                ecgMetData, ecgMaskMetData, ECG_MET_WINDOW_LEN,
                &ecgMetResults
            );

            if (appState.arrMode == ArrhythmiaModeDsp) {
                ecgMetResults.arrhythmiaLabel = ecgMetResults.hr < 40 ? ECG_ARR_SB : ecgMetResults.hr > 100 ? ECG_ARR_GSVT : ECG_ARR_SR;
                ns_lp_printf("HR: %f\n", ecgMetResults.hr);
            } else if (appState.arrMode == ArrhythmiaModeAi) {
                ecgMetResults.arrhythmiaLabel = ecg_arrhythmia_inference(ecgMetData, ECG_ARR_THRESHOLD);
            } else {
                ecgMetResults.arrhythmiaLabel = 0;
            }
            deltaUs = ns_us_ticker_read(&ecgTimerCfg) - tickUs;
            ecgMetResults.arrhythmiaIps = 1000000.0/deltaUs;

            // Store metrics
            ringbuffer_seek(&rbEcgMet, ECG_MET_VALID_LEN);
            ringbuffer_seek(&rbEcgMaskMet, ECG_MET_VALID_LEN);

            // Broadcast metrics
            send_ecg_metrics();
            ns_lp_printf("<ECG METRICS Time: %d (err=%d) >\n", deltaUs/1000, err);
        }

        ///////////////////////////////////////////////////////////////////////
        // Send ECG Signals to TIO
        ///////////////////////////////////////////////////////////////////////
        send_ecg_signals();

        // Try to maintain 100ms loop
        deltaUs = ns_us_ticker_read(&ecgTimerCfg);
        if (deltaUs < 100000) {
            delayUs = 100000 - deltaUs;
            vTaskDelay(pdMS_TO_TICKS(delayUs/1000));
        }
    }
}

void
PpgProcessTask(void *pvParameters) {
    uint32_t err = 0;
    uint32_t delayUs = 0, tickUs = 0;
    uint32_t deltaUs = 0;
    size_t numSamples = 0;

    while (true) {
        err = 0;
        ns_timer_clear(&ppgTimerCfg);

        ///////////////////////////////////////////////////////////////////////
        // PPG PREPROCESSING BLOCK
        ///////////////////////////////////////////////////////////////////////
        numSamples = MIN(
            ringbuffer_len(&rbPpg1Sensor),
            ringbuffer_len(&rbPpg2Sensor)
        );
        for (size_t i = 0; i < numSamples/PPG_DS_RATE; i++) {
            ringbuffer_seek(&rbPpg1Sensor, PPG_DS_RATE - 1);
            ringbuffer_transfer(&rbPpg1Sensor, &rbPpg1Den, 1);
            ringbuffer_seek(&rbPpg2Sensor, PPG_DS_RATE - 1);
            ringbuffer_transfer(&rbPpg2Sensor, &rbPpg2Den, 1);
        }

        sensor_read_spo2(&ppgMetResults.spo2, &ppgMetResults.pr, &ppgMetResults.qos);

        ///////////////////////////////////////////////////////////////////////
        // PPG DENOSING BLOCK
        ///////////////////////////////////////////////////////////////////////
        if (MIN(ringbuffer_len(&rbPpg1Den), ringbuffer_len(&rbPpg2Den)) >= PPG_DEN_WINDOW_LEN) {
            err = 0;
            tickUs = ns_us_ticker_read(&ppgTimerCfg);
            ringbuffer_peek(&rbPpg1Den, ppg1DenInout, PPG_DEN_WINDOW_LEN);
            ringbuffer_peek(&rbPpg2Den, ppg2DenInout, PPG_DEN_WINDOW_LEN);
            // pk_standardize_f32(ppg1DenInout, ppg1DenInout, PPG_DEN_WINDOW_LEN, NORM_STD_EPS);
            // pk_standardize_f32(ppg2DenInout, ppg2DenInout, PPG_DEN_WINDOW_LEN, NORM_STD_EPS);
            // err = ppg_denoise_inference(ppg1DenInout, ppg1DenInout, 0, PPG_DEN_THRESHOLD);
            // err = ppg_denoise_inference(ppg2DenInout, ppg2DenInout, 0, PPG_DEN_THRESHOLD);
            ringbuffer_push(&rbPpg1Seg, &ppg1DenInout[PPG_DEN_PAD_LEN], PPG_DEN_VALID_LEN);
            ringbuffer_push(&rbPpg2Seg, &ppg2DenInout[PPG_DEN_PAD_LEN], PPG_DEN_VALID_LEN);
            ringbuffer_seek(&rbPpg1Den, PPG_DEN_VALID_LEN);
            ringbuffer_seek(&rbPpg2Den, PPG_DEN_VALID_LEN);
            deltaUs = ns_us_ticker_read(&ppgTimerCfg) - tickUs;
            ns_lp_printf("<PPG DENOISE Time: %d (err=%d) >\n", deltaUs/1000, err);
        }

        ///////////////////////////////////////////////////////////////////////
        // PPG SEGMENTATION BLOCK
        ///////////////////////////////////////////////////////////////////////
        else if (MIN(ringbuffer_len(&rbPpg1Seg), ringbuffer_len(&rbPpg2Seg)) >= PPG_SEG_WINDOW_LEN) {
            err = 0;
            tickUs = ns_us_ticker_read(&ppgTimerCfg);
            ringbuffer_peek(&rbPpg1Seg, ppg1SegInout, PPG_SEG_WINDOW_LEN);
            ringbuffer_peek(&rbPpg2Seg, ppg2SegInout, PPG_SEG_WINDOW_LEN);
            // pk_standardize_f32(ppg1SegInout, ppg1SegInout, PPG_SEG_WINDOW_LEN, NORM_STD_EPS);
            // pk_standardize_f32(ppg2SegInout, ppg2SegInout, PPG_SEG_WINDOW_LEN, NORM_STD_EPS);
            // err = ppg_segmentation_inference(ppg1SegInout, ppg1SegMask, 0, PPG_SEG_THRESHOLD);
            // err = ppg_segmentation_inference(ppg2SegInout, ppg2SegMask, 0, PPG_SEG_THRESHOLD);
            ringbuffer_push(&rbPpg1Tx, &ppg1SegInout[PPG_SEG_PAD_LEN], PPG_SEG_VALID_LEN);
            ringbuffer_push(&rbPpg2Tx, &ppg2SegInout[PPG_SEG_PAD_LEN], PPG_SEG_VALID_LEN);

            ringbuffer_push(&rbPpg1Met, &ppg1SegInout[PPG_SEG_PAD_LEN], PPG_SEG_VALID_LEN);
            ringbuffer_push(&rbPpg2Met, &ppg2SegInout[PPG_SEG_PAD_LEN], PPG_SEG_VALID_LEN);

            ringbuffer_seek(&rbPpg1Seg, PPG_SEG_VALID_LEN);
            ringbuffer_seek(&rbPpg2Seg, PPG_SEG_VALID_LEN);
            deltaUs = ns_us_ticker_read(&ppgTimerCfg) - tickUs;
            ns_lp_printf("<PPG SEGMENT Time: %d (err=%d) >\n", deltaUs/1000, err);
        }

        ///////////////////////////////////////////////////////////////////////
        // PPG METRICS BLOCK
        ///////////////////////////////////////////////////////////////////////
        else if (MIN(ringbuffer_len(&rbPpg1Met), ringbuffer_len(&rbPpg2Met)) >= PPG_MET_WINDOW_LEN) {
            err = 0;
            tickUs = ns_us_ticker_read(&ppgTimerCfg);

            ringbuffer_peek(&rbPpg1Met, ppg1MetData, ECG_MET_WINDOW_LEN);
            ringbuffer_peek(&rbPpg2Met, ppg2MetData, ECG_MET_WINDOW_LEN);

            send_ppg_metrics();
            deltaUs = ns_us_ticker_read(&ppgTimerCfg) - tickUs;

            ringbuffer_seek(&rbPpg1Met, PPG_MET_VALID_LEN);
            ringbuffer_seek(&rbPpg2Met, PPG_MET_VALID_LEN);

            ns_lp_printf("<PPG TX Time: %d (err=%d) SpO2 = %0.1f, qos=%0.1f>\n", deltaUs/1000, err, ppgMetResults.spo2, ppgMetResults.qos);
        }

        ///////////////////////////////////////////////////////////////////////
        // Send PPG Signals to TIO
        ///////////////////////////////////////////////////////////////////////
        send_ppg_signals();

        // Try to maintain 100ms loop
        deltaUs = ns_us_ticker_read(&ppgTimerCfg);
        if (deltaUs < 100000) {
            delayUs = 100000 - deltaUs;
            vTaskDelay(pdMS_TO_TICKS(delayUs/1000));
        }
    }
}


void
SetupTask(void *pvParameters)
{
    xTaskCreate(EcgProcessTask, "EcgProcessTask", 1024, 0, 1, &ecgTaskHandle);
    xTaskCreate(PpgProcessTask, "PpgProcessTask", 2048, 0, 1, &ppgTaskHandle);
    vTaskSuspend(NULL);
    while (1) { };
}


int
main(void)
{
    uint32_t value;
    sensorCtx.inputSource = appState.inputSource;
    nsPwrCfg.eAIPowerMode = appState.speedMode ? NS_MAXIMUM_PERF : NS_MINIMUM_PERF;

    tioUsbCtx.slot_update_cb = &received_slot_data;
    tioUsbCtx.uio_update_cb = &received_uio_state;

    NS_TRY(ns_core_init(&nsCoreCfg), "Core Init failed.\b");
    NS_TRY(ns_power_config(&nsPwrCfg), "Power Init Failed\n");
    ns_delay_us(200000); // 200ms
    // NS_TRY(ns_i2c_interface_init(&nsI2cCfg, I2C_SPEED_HZ), "I2C Init Failed\n");
    NS_TRY(ns_spi_interface_init(&nsSpiCfg, AM_HAL_IOM_2MHZ, AM_HAL_IOM_SPI_MODE_2), "SPI Init Failed\n");

    NS_TRY(rtos_time_init(), "RTOS Timer Init failed.\n");
    NS_TRY(ns_timer_init(&ecgTimerCfg), "Timer Init failed.\n");
    NS_TRY(ns_timer_init(&ppgTimerCfg), "Timer 2 Init failed.\n");
    // NS_TRY(pmic_init(), "PMIC Setup failed.\n");
    NS_TRY(sensor_init(&sensorCtx), "Sensor Init failed.\n");
    NS_TRY(tflm_init(), "TFLM Init Failed\n");
    NS_TRY(ecg_denoise_init(), "ECG Segmentation Init Failed\n");
    NS_TRY(ecg_segmentation_init(), "ECG Segmentation Init Failed\n");
    NS_TRY(ecg_arrhythmia_init(), "ECG Arrhythmia Init Failed\n");
    NS_TRY(metrics_init(&metricsCfg), "Metrics Init Failed\n");
    NS_TRY(tio_usb_init(&tioUsbCtx), "TIO Init Failed\n");
    ns_delay_us(200000);
    // pmic_start(0);
    ns_itm_printf_enable();
    ns_interrupt_master_enable();
    ns_delay_us(200000);
    NS_TRY(sensor_configure(), "Sensor Configure failed.\n");
    NS_TRY(sensor_start(), "Sensor Start failed.\n");
    // xTaskCreate(SetupTask, "Setup", 512, 0, 3, &appSetupTask);
    xTaskCreate(EcgProcessTask, "EcgProcessTask", 1024, 0, 1, &ecgTaskHandle);
    xTaskCreate(PpgProcessTask, "PpgProcessTask", 1024, 0, 1, &ppgTaskHandle);
    xTaskCreate(CpuProcessTask, "CpuProcessTask",  512, 0, 1, &cpuTaskHandle);
    vTaskStartScheduler();
    while (1) {};
}
