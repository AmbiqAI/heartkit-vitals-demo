/**
 * @file main.cc
 * @brief HeartKit vitals demo (NSX port, phase 6: full app orchestration).
 *
 * Full parity port of legacy heartkit-vitals-demo/src/main.cc onto the NSX
 * SDK: AS7058 PPG+ECG sensing (nsx-as7058), DSP (nsx-physiokit) and
 * AI/TFLM (nsx-helia-rt) ECG denoise/segmentation/arrhythmia pipelines
 * selectable at runtime via app_state_t mode switches (denoise/seg/
 * arrhythmia mode, input source, noise levels, CPU speed mode), a
 * FreeRTOS-runtime-stats-driven CPU utilization monitor, and TileIO USB
 * streaming (nsx-tileio-usb) with a full 3-channel ECG / metrics / CPU
 * packet layout matching legacy, plus host->device UIO mode control.
 *
 * Known, documented scope gaps vs legacy (see plan.md phase 6 notes and
 * sensor.h):
 *  - sensor.c does not yet implement legacy's canned-stimulus-substitution
 *    ISR path (playing back ecg_stimulus/ppg1_stimulus/ppg2_stimulus in
 *    place of live AS7058 FIFO data when appState.inputSource selects a
 *    non-live slot) -- setting a non-live input source via UIO currently
 *    has no effect on the sampled data (always live). Noise injection
 *    (nstdb_add_*_noise) and denoise-quality cosine-similarity, which
 *    legacy only computes in non-live/synthetic mode, are wired the same
 *    way here for API parity but are effectively inert while
 *    sensorCtx.inputSource stays at its LIVE_INPUT_MODE default.
 *  - True dual-wavelength PPG/SpO2 remains n/a: the active AS7058
 *    "click_ppg_ecg" profile only streams one PPG wavelength (see
 *    store.h). PPG signal streaming is 1ch instead of legacy's 2ch, and
 *    spo2 is always reported as 0 (n/a).
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "am_mcu_apollo.h"

#include "nsx_core.h"
#include "nsx_freertos.h"
#include "nsx_i2c.h"
#include "nsx_power.h"
#include "nsx_spi.h"

#include "pk_ecg.h"
#include "pk_filter.h"
#include "pk_math.h"
#include "pk_ppg.h"

#include "constants.h"
#include "metrics.h"
#include "nstdb_noise.h"
#include "ringbuffer.h"
#include "sensor.h"
#include "store.h"

#include "tflm.h"
#include "ecg_arrhythmia.h"
#include "ecg_denoise.h"
#include "ecg_segmentation.h"

#include "tio_usb.h"

static TaskHandle_t sensorIrqTaskHandle;
static TaskHandle_t ecgProcessTaskHandle;
static TaskHandle_t ppgProcessTaskHandle;
static TaskHandle_t cpuProcessTaskHandle;
static TaskHandle_t tioProcessTaskHandle;
static TaskHandle_t reportTaskHandle;

static TaskStatus_t xTaskDetails[10];

static QueueHandle_t g_tioTxQueue = NULL;
static volatile uint32_t g_tio_tx_queue_drops = 0;
static const TickType_t kTioTxTaskPollTicks = pdMS_TO_TICKS(10);
static const uint32_t kCpuStatsSamplePeriodMs = 100;
static const uint32_t kCpuStatsPublishPeriodMs = 1000;
static const uint32_t kCpuStatsRollingSeconds = 30;

///////////////////////////////////////////////////////////////////////////////
// DWT cycle counter (per-stage IPS timing)
///////////////////////////////////////////////////////////////////////////////
//
// nsx-core does not (yet) expose a microsecond-ticker peripheral wrapper
// equivalent to legacy's ns_timer_config_t/ns_us_ticker_read(), so per-stage
// ECG/PPG denoise/segment/arrhythmia latency (IPS = inferences-per-second)
// is measured directly via the Cortex-M DWT cycle counter, converted to
// microseconds using SystemCoreClock -- the same technique already proven
// in phase 4's AiModelDemoTask.

extern "C" uint32_t RTOS_AppConfigureTimerForRuntimeStats(void);
extern "C" uint32_t RTOS_AppGetRuntimeCounterValueFromISR(void);

static inline void
dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t
dwt_cycles(void)
{
    return DWT->CYCCNT;
}

static inline uint32_t
dwt_delta_us(uint32_t startCycles)
{
    uint32_t deltaCycles = dwt_cycles() - startCycles;
    return deltaCycles / (SystemCoreClock / 1000000);
}

///////////////////////////////////////////////////////////////////////////////
// FreeRTOS runtime-stats timer (am_hal_timer, RTOS_TIMER channel)
///////////////////////////////////////////////////////////////////////////////
//
// Drives configGENERATE_RUN_TIME_STATS (FreeRTOSConfig.h) so CpuProcessTask
// can read per-task run-time counters via uxTaskGetSystemState(). Ported
// near-verbatim from legacy main.cc's rtos_time_init/rtos_ticker_read/
// rtos_timer_clear + RTOS_AppConfigureTimerForRuntimeStats/
// RTOS_AppGetRuntimeCounterValueFromISR -- this uses the AmbiqSuite HAL
// (am_hal_timer_*) directly, which is board/SoC-portable and not
// neuralSPOT-specific, so it needs no NSX wrapper.

static volatile uint32_t g_rtos_stat_timer_cnt = 0;

extern "C" uint32_t
rtos_time_init(void)
{
    uint32_t timerNum = RTOS_TIMER;
    uint32_t status;
    g_rtos_stat_timer_cnt = 0;
    am_hal_timer_config_t rtosTimerConfig;
    am_hal_timer_default_config_set(&rtosTimerConfig);
    // 4096/(96 megahertz)*6 = 256us precision
    rtosTimerConfig.eInputClock = AM_HAL_TIMER_CLOCK_HFRC_DIV4K;
    rtosTimerConfig.eTriggerSource = AM_HAL_TIMER_TRIGGER_TMR4_OUT1;
    status = am_hal_timer_config(timerNum, &rtosTimerConfig);
    am_hal_timer_clear(timerNum);
    return status;
}

static inline uint32_t
rtos_ticker_read(void)
{
    return am_hal_timer_read(RTOS_TIMER);
}

static inline void
rtos_timer_clear(void)
{
    am_hal_timer_clear(RTOS_TIMER);
}

extern "C" uint32_t
RTOS_AppConfigureTimerForRuntimeStats(void)
{
    g_rtos_stat_timer_cnt = 0;
    return 0;
}

extern "C" uint32_t
RTOS_AppGetRuntimeCounterValueFromISR(void)
{
    // Check for overflow
    if (g_rtos_stat_timer_cnt > 0x7FFFFFFF) {
        g_rtos_stat_timer_cnt = 0;
        rtos_timer_clear();
    }
    g_rtos_stat_timer_cnt = rtos_ticker_read();
    return g_rtos_stat_timer_cnt;
}

///////////////////////////////////////////////////////////////////////////////
// TileIO USB streaming
///////////////////////////////////////////////////////////////////////////////
//
// ECG (slot 0): type 0 = raw+denoised+mask (3ch, matches legacy), type 1 =
// HR/HRV/denoise-cossim/arrhythmia/IPS/QoS metrics. PPG (slot 1): type 0 =
// single-wavelength signal (see store.h for why only 1ch), type 1 = PR/
// QoS metrics (spo2 always 0, n/a). CPU (slot 2): type 0 = per-task
// utilization percentages, type 1 = overall CPU%/battery-days/avg AI IPS.
// UIO carries the 8 app_state_t mode-select bytes (constants.h
// TIO_UIO_*_IDX) in both directions.

static void received_slot_data(uint8_t slot, uint8_t slot_type, const uint8_t *data, uint32_t length);
static void received_uio_state(const uint8_t *data, uint32_t length);

static tio_usb_context_t tioUsbCtx = {
    .uio_update_cb = &received_uio_state,
    .slot_update_cb = &received_slot_data,
    .manufacturer = "Ambiq",
    .product = "heartkit-vitals-demo",
    .serial = "NSX-HKV-0001",
    .cdc_interface = "NSX CDC",
    .vendor_interface = "TileIO Vendor",
    .webusb_url = "tileio.local",
    .vid = TIO_USB_VENDOR_ID,
    .pid = TIO_USB_PRODUCT_ID,
};

/**
 * @brief Flush pipeline buffers (called on host reconnect to avoid streaming
 * stale data accumulated while no host was attached).
 */
static rb_config_t *const g_pipeline_flush_buffers[] = {
    &rbEcgSensor, &rbEcgDen,   &rbEcgRawSeg, &rbEcgSeg,     &rbEcgMet,   &rbEcgMaskMet,
    &rbEcgRawTx,  &rbEcgDenTx, &rbEcgMaskTx, &rbPpg1Sensor, &rbPpg1Met,  &rbPpg1Tx,
    &rbEcgCpuTx,  &rbPpgCpuTx, &rbTotalCpuTx,
};

static void
flush_pipeline(void)
{
    for (size_t i = 0; i < (sizeof(g_pipeline_flush_buffers) / sizeof(g_pipeline_flush_buffers[0])); ++i) {
        ringbuffer_flush(g_pipeline_flush_buffers[i]);
    }
    if (g_tioTxQueue != NULL) {
        xQueueReset(g_tioTxQueue);
    }
}

static bool
enqueue_tio_packet(const uint8_t packet[TIO_USB_PACKET_LEN])
{
    BaseType_t queued = pdFALSE;
    if (g_tioTxQueue == NULL) {
        g_tio_tx_queue_drops++;
        return false;
    }

    if (xPortIsInsideInterrupt()) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        queued = xQueueSendFromISR(g_tioTxQueue, packet, &xHigherPriorityTaskWoken);
        if (pdTRUE == xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    } else {
        queued = xQueueSend(g_tioTxQueue, packet, 0);
    }

    if (queued != pdTRUE) {
        g_tio_tx_queue_drops++;
        return false;
    }
    return true;
}

static bool
pack_and_enqueue_tio_packet(uint8_t slot, uint8_t slot_type, const void *payload, uint32_t payload_len)
{
    uint8_t packet[TIO_USB_PACKET_LEN];
    if (tio_usb_pack_slot_data(slot, slot_type, (const uint8_t *)payload, payload_len, packet) != 0) {
        return false;
    }
    return enqueue_tio_packet(packet);
}

static volatile bool g_tio_available = false;

static void
check_tio_state(void)
{
    bool available = tio_usb_tx_available() != 0;
    if (available != g_tio_available) {
        g_tio_available = available;
        if (g_tio_available) {
            nsx_printf("[tio] host connected\n");
            flush_pipeline();
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// App mode switches (host UIO -> app_state_t)
///////////////////////////////////////////////////////////////////////////////

static void
set_input_source(uint8_t source)
{
    source = MIN(source, NUM_INPUT_PTS);
    if (appState.inputSource != source) {
        appState.inputSource = source;
        sensorCtx.inputSource = source;
        nsx_printf("[app] input source: %d\n", (int)sensorCtx.inputSource);
    }
}

static void
set_noise_inputs(uint8_t bw, uint8_t ma, uint8_t em)
{
    appState.bwNoiseLevel = bw;
    appState.maNoiseLevel = ma;
    appState.emNoiseLevel = em;
    nsx_printf("[app] noise levels: bw=%d ma=%d em=%d\n", (int)appState.bwNoiseLevel, (int)appState.maNoiseLevel,
               (int)appState.emNoiseLevel);
}

static void
set_denoise_mode(uint8_t mode)
{
    mode = MIN(mode, 2);
    if (appState.denoiseMode != mode) {
        appState.denoiseMode = mode;
        nsx_printf("[app] denoise mode: %d\n", (int)appState.denoiseMode);
    }
}

static void
set_segmentation_mode(uint8_t mode)
{
    mode = MIN(mode, 2);
    if (appState.segMode != mode) {
        appState.segMode = mode;
        nsx_printf("[app] segmentation mode: %d\n", (int)appState.segMode);
    }
}

static void
set_arrhythmia_mode(uint8_t mode)
{
    mode = MIN(mode, 2);
    if (appState.arrMode != mode) {
        appState.arrMode = mode;
        nsx_printf("[app] arrhythmia mode: %d\n", (int)appState.arrMode);
    }
}

static void
set_speed_mode(uint8_t mode)
{
    mode = MIN(mode, 1);
    if (appState.speedMode != mode) {
        appState.speedMode = mode;
        nsx_power_set_performance_mode(appState.speedMode ? NSX_POWER_PERF_HIGH : NSX_POWER_PERF_LOW);
        nsx_printf("[app] CPU speed mode: %d\n", (int)appState.speedMode);
    }
}

static void
send_uio_state(void)
{
    uint8_t uioBuffer[8];
    uioBuffer[TIO_UIO_INPUT_SEL_IDX] = appState.inputSource;
    uioBuffer[TIO_UIO_BW_NOISE_IDX] = appState.bwNoiseLevel;
    uioBuffer[TIO_UIO_MA_NOISE_IDX] = appState.maNoiseLevel;
    uioBuffer[TIO_UIO_EM_NOISE_IDX] = appState.emNoiseLevel;
    uioBuffer[TIO_UIO_SPEED_MODE_IDX] = appState.speedMode;
    uioBuffer[TIO_UIO_DEN_MODE_IDX] = appState.denoiseMode;
    uioBuffer[TIO_UIO_SEG_MODE_IDX] = appState.segMode;
    uioBuffer[TIO_UIO_ARR_MODE_IDX] = appState.arrMode;
    tio_usb_send_uio_state(uioBuffer, sizeof(uioBuffer));
}

static void
received_slot_data(uint8_t slot, uint8_t slot_type, const uint8_t *data, uint32_t length)
{
    // No host->device slot data expected.
    (void)slot;
    (void)slot_type;
    (void)data;
    (void)length;
}

static void
received_uio_state(const uint8_t *data, uint32_t length)
{
    if (length < 8) {
        return;
    }
    set_input_source(data[TIO_UIO_INPUT_SEL_IDX]);
    set_noise_inputs(data[TIO_UIO_BW_NOISE_IDX], data[TIO_UIO_MA_NOISE_IDX], data[TIO_UIO_EM_NOISE_IDX]);
    set_speed_mode(data[TIO_UIO_SPEED_MODE_IDX]);
    set_denoise_mode(data[TIO_UIO_DEN_MODE_IDX]);
    set_segmentation_mode(data[TIO_UIO_SEG_MODE_IDX]);
    set_arrhythmia_mode(data[TIO_UIO_ARR_MODE_IDX]);
    send_uio_state();
}

///////////////////////////////////////////////////////////////////////////////
// TIO packet senders
///////////////////////////////////////////////////////////////////////////////

static void
send_ecg_signals(void)
{
    uint8_t buffer[240];
    float32_t rawVal, denVal;
    uint16_t maskVal;
    int16_t txVal;
    uint32_t length;
    size_t numSamples = MIN3(ringbuffer_len(&rbEcgRawTx), ringbuffer_len(&rbEcgDenTx), ringbuffer_len(&rbEcgMaskTx));
    if (numSamples == 0) {
        return;
    }
    numSamples = MIN(numSamples, sizeof(buffer) / (3 * sizeof(int16_t)));
    length = 0;
    for (size_t i = 0; i < numSamples; i++) {
        ringbuffer_pop(&rbEcgMaskTx, &maskVal, 1);
        memcpy(&buffer[length], &maskVal, sizeof(uint16_t));
        length += sizeof(uint16_t);
        ringbuffer_pop(&rbEcgRawTx, &rawVal, 1);
        txVal = (int16_t)CLIP(TIO_SLOT0_SCALE * rawVal, -32768, 32767);
        memcpy(&buffer[length], &txVal, sizeof(int16_t));
        length += sizeof(int16_t);
        ringbuffer_pop(&rbEcgDenTx, &denVal, 1);
        txVal = (int16_t)CLIP(TIO_SLOT0_SCALE * denVal, -32768, 32767);
        memcpy(&buffer[length], &txVal, sizeof(int16_t));
        length += sizeof(int16_t);
    }
    pack_and_enqueue_tio_packet(0, 0, buffer, length);
}

static void
send_ecg_metrics(void)
{
    float32_t buffer[16];
    buffer[0] = ecgMetResults.hr;
    buffer[1] = ecgMetResults.hrv;
    buffer[2] = ecgMetResults.denoiseCossim;
    buffer[3] = ecgMetResults.arrhythmiaLabel;
    buffer[4] = ecgMetResults.denoiseIps;
    buffer[5] = ecgMetResults.segmentIps;
    buffer[6] = ecgMetResults.arrhythmiaIps;
    buffer[7] = ecgMetResults.qos;
    buffer[8] = ecgMetResults.denoiseuIpspw;
    buffer[9] = ecgMetResults.segmentuIpspw;
    buffer[10] = ecgMetResults.arrhythmiaIpspw;
    pack_and_enqueue_tio_packet(0, 1, buffer, 11 * sizeof(float32_t));
}

static void
send_ppg_signals(void)
{
    uint8_t buffer[240];
    float32_t val;
    int16_t txVal;
    uint32_t length;
    uint8_t qos = (uint8_t)(ppgMetResults.qos / 25);
    uint16_t mask = (uint16_t)(qos << SIG_MASK_QOS_OFFSET);
    size_t numSamples = ringbuffer_len(&rbPpg1Tx);
    if (numSamples == 0) {
        return;
    }
    numSamples = MIN(numSamples, sizeof(buffer) / (sizeof(uint16_t) + sizeof(int16_t)));
    length = 0;
    for (size_t i = 0; i < numSamples; i++) {
        memcpy(&buffer[length], &mask, sizeof(uint16_t));
        length += sizeof(uint16_t);
        ringbuffer_pop(&rbPpg1Tx, &val, 1);
        txVal = (int16_t)CLIP(val, -32768.0f, 32767.0f);
        memcpy(&buffer[length], &txVal, sizeof(int16_t));
        length += sizeof(int16_t);
    }
    pack_and_enqueue_tio_packet(1, 0, buffer, length);
}

static void
send_ppg_metrics(void)
{
    float32_t buffer[4];
    buffer[0] = ppgMetResults.pr;
    buffer[1] = ppgMetResults.spo2;
    buffer[2] = ppgMetResults.qos;
    pack_and_enqueue_tio_packet(1, 1, buffer, 3 * sizeof(float32_t));
}

static void
send_cpu_signals(void)
{
    uint8_t buffer[240];
    float32_t val;
    uint32_t length;
    uint16_t mask = (uint16_t)(SIG_QOS_GOOD << SIG_MASK_QOS_OFFSET);
    size_t numSamples = MIN3(ringbuffer_len(&rbEcgCpuTx), ringbuffer_len(&rbPpgCpuTx), ringbuffer_len(&rbTotalCpuTx));
    if (numSamples == 0) {
        return;
    }
    numSamples = MIN(numSamples, sizeof(buffer) / (sizeof(uint16_t) + 3 * sizeof(float32_t)));
    length = 0;
    for (size_t i = 0; i < numSamples; i++) {
        memcpy(&buffer[length], &mask, sizeof(uint16_t));
        length += sizeof(uint16_t);
        ringbuffer_pop(&rbEcgCpuTx, &val, 1);
        memcpy(&buffer[length], &val, sizeof(float32_t));
        length += sizeof(float32_t);
        ringbuffer_pop(&rbPpgCpuTx, &val, 1);
        memcpy(&buffer[length], &val, sizeof(float32_t));
        length += sizeof(float32_t);
        ringbuffer_pop(&rbTotalCpuTx, &val, 1);
        memcpy(&buffer[length], &val, sizeof(float32_t));
        length += sizeof(float32_t);
    }
    pack_and_enqueue_tio_packet(2, 0, buffer, length);
}

static void
send_cpu_metrics(void)
{
    float32_t buffer[3];
    buffer[0] = appMetResults.cpuPercUtil;
    buffer[1] = appMetResults.batteryDays;
    buffer[2] = appMetResults.avgAiIps;
    pack_and_enqueue_tio_packet(2, 1, buffer, 3 * sizeof(float32_t));
}

///////////////////////////////////////////////////////////////////////////////
// Sensor IRQ deferral
///////////////////////////////////////////////////////////////////////////////

void
SensorIrqTask(void *pvParameters)
{
    (void)pvParameters;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        sensor_process_irq_events();
    }
}

///////////////////////////////////////////////////////////////////////////////
// ECG process task
///////////////////////////////////////////////////////////////////////////////
//
// Full pipeline: downsample -> (synthetic-mode-only) noise injection ->
// DSP/AI denoise (mode-gated) -> DSP/AI segmentation (mode-gated) ->
// DSP/AI arrhythmia (mode-gated) -> metrics (HR/HRV) -> TileIO TX. Mirrors
// legacy's EcgProcessTask 1:1 modulo the sensor.c stimulus-substitution gap
// documented at the top of this file.

void
EcgProcessTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t err = 0;
    uint32_t tickStart;
    uint32_t numPeaks;
    size_t numSamples;

    while (true) {
        err = 0;

        ///////////////////////////////////////////////////////////////////
        // ECG PREPROCESSING: downsample sensor rate to target rate.
        ///////////////////////////////////////////////////////////////////
        numSamples = ringbuffer_len(&rbEcgSensor);
        for (size_t i = 0; i < numSamples / ECG_DS_RATE; i++) {
            ringbuffer_seek(&rbEcgSensor, ECG_DS_RATE - 1);
            ringbuffer_transfer(&rbEcgSensor, &rbEcgDen, 1);
        }

        ///////////////////////////////////////////////////////////////////
        // ECG DENOISE
        ///////////////////////////////////////////////////////////////////
        if (ringbuffer_len(&rbEcgDen) >= ECG_DEN_WINDOW_LEN) {
            tickStart = dwt_cycles();
            ringbuffer_peek(&rbEcgDen, ecgDenInout, ECG_DEN_WINDOW_LEN);

            pk_standardize_f32(ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, NORM_STD_EPS);

            // Keep a noise-free reference for cosine-similarity scoring below.
            memcpy(ecgDenNoise, ecgDenInout, ECG_DEN_WINDOW_LEN * sizeof(float32_t));

            // Synthetic-mode-only noise injection (see file header: inert
            // in live mode since sensor.c doesn't yet substitute canned
            // stimulus data for non-live input sources).
            if (sensorCtx.inputSource < NUM_INPUT_PTS) {
                nstdb_add_bw_noise(ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, (float32_t)appState.bwNoiseLevel * 2.0e-5f);
                nstdb_add_ma_noise(ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, (float32_t)appState.maNoiseLevel * 1.0e-5f);
                nstdb_add_em_noise(ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, (float32_t)appState.emNoiseLevel * 1.0e-5f);
            }

            // Raw (possibly noisy) signal feeds the "raw" TX channel.
            ringbuffer_push(&rbEcgRawSeg, &ecgDenInout[ECG_DEN_PAD_LEN], ECG_DEN_VALID_LEN);

            if (appState.denoiseMode == DenoiseModeDsp || appState.denoiseMode == DenoiseModeAi) {
                err = pk_apply_biquad_filtfilt_f32(&ecgFilterCtx, ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN, ecgDenScratch);
            }
            if (appState.denoiseMode == DenoiseModeAi) {
                err = ecg_denoise_inference(ecgDenInout, ecgDenInout, 0, ECG_DEN_THRESHOLD);
            } else {
                err = 0;
            }

            if (sensorCtx.inputSource < NUM_INPUT_PTS) {
                cosine_similarity_f32(&ecgDenInout[ECG_DEN_PAD_LEN], &ecgDenNoise[ECG_DEN_PAD_LEN], ECG_DEN_VALID_LEN,
                                       &ecgMetResults.denoiseCossim);
            } else {
                // Live mode: no noise-free reference to compare against.
                ecgMetResults.denoiseCossim = 1.0f;
            }
            ecgMetResults.denoiseCossim *= 100.0f;

            ringbuffer_push(&rbEcgSeg, &ecgDenInout[ECG_DEN_PAD_LEN], ECG_DEN_VALID_LEN);
            ringbuffer_seek(&rbEcgDen, ECG_DEN_VALID_LEN);

            ecgMetResults.denoiseIps = 1.0e6f / (float32_t)dwt_delta_us(tickStart);
            ecgMetResults.denoiseuIpspw = 1.0e3f * ecgMetResults.denoiseIps / AVG_INFERENCE_POWER;
#if EN_APP_TIMING_LOGS
            nsx_printf("[ecg] denoise err=%lu\n", (unsigned long)err);
#endif
        }

        ///////////////////////////////////////////////////////////////////
        // ECG SEGMENTATION
        ///////////////////////////////////////////////////////////////////
        else if (ringbuffer_len(&rbEcgSeg) >= ECG_SEG_WINDOW_LEN) {
            tickStart = dwt_cycles();
            ringbuffer_peek(&rbEcgSeg, ecgSegInout, ECG_SEG_WINDOW_LEN);

            if (appState.segMode == SegmentationModeDsp) {
                numPeaks = pk_ecg_find_peaks_f32(&ecgPkPeakCtx, ecgSegInout, ECG_SEG_WINDOW_LEN, peaksMetrics, ecgSegMask);
                for (size_t i = 0; i < ECG_SEG_WINDOW_LEN; i++) {
                    ecgSegMask[i] = ecgSegMask[i] > 0 ? ECG_SEG_QRS : ECG_SEG_NONE;
                }
                for (size_t i = 0; i < numPeaks; i++) {
                    ecgSegMask[peaksMetrics[i]] |= (ECG_FID_PEAK_QRS << ECG_MASK_FID_PEAK_OFFSET);
                }
                err = 0;
            } else if (appState.segMode == SegmentationModeAi) {
                err = ecg_segmentation_inference(ecgSegInout, ecgSegMask, 0, ECG_SEG_THRESHOLD, &ecgMetResults.qos);
            } else {
                err = 0;
                for (size_t i = 0; i < ECG_SEG_WINDOW_LEN; i++) {
                    ecgSegMask[i] = ECG_SEG_NONE;
                }
            }

            ringbuffer_transfer(&rbEcgRawSeg, &rbEcgRawTx, ECG_SEG_VALID_LEN);
            ringbuffer_push(&rbEcgDenTx, &ecgSegInout[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);
            ringbuffer_push(&rbEcgMaskTx, &ecgSegMask[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);

            ringbuffer_push(&rbEcgMet, &ecgSegInout[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);
            ringbuffer_push(&rbEcgMaskMet, &ecgSegMask[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);

            ringbuffer_seek(&rbEcgSeg, ECG_SEG_VALID_LEN);

            ecgMetResults.segmentIps = 1.0e6f / (float32_t)dwt_delta_us(tickStart);
            ecgMetResults.segmentuIpspw = 1.0e3f * ecgMetResults.segmentIps / AVG_INFERENCE_POWER;
#if EN_APP_TIMING_LOGS
            nsx_printf("[ecg] segment err=%lu\n", (unsigned long)err);
#endif
        }

        ///////////////////////////////////////////////////////////////////
        // ECG ARRHYTHMIA + METRICS
        ///////////////////////////////////////////////////////////////////
        else if (MIN(ringbuffer_len(&rbEcgMet), ringbuffer_len(&rbEcgMaskMet)) >= ECG_MET_WINDOW_LEN) {
            tickStart = dwt_cycles();
            ringbuffer_peek(&rbEcgMet, ecgMetData, ECG_MET_WINDOW_LEN);
            ringbuffer_peek(&rbEcgMaskMet, ecgMaskMetData, ECG_MET_WINDOW_LEN);

            err = metrics_capture_ecg(&metricsCfg, ecgMetData, ecgMaskMetData, ECG_MET_WINDOW_LEN, &ecgMetResults);

            if (appState.arrMode == ArrhythmiaModeDsp) {
                ecgMetResults.arrhythmiaLabel =
                    ecgMetResults.hr < 40 ? ECG_ARR_SB : ecgMetResults.hr > 100 ? ECG_ARR_GSVT : ECG_ARR_SR;
            } else if (appState.arrMode == ArrhythmiaModeAi) {
                ecgMetResults.arrhythmiaLabel = ecg_arrhythmia_inference(ecgMetData, ECG_ARR_THRESHOLD);
            } else {
                ecgMetResults.arrhythmiaLabel = 0;
            }

            ringbuffer_seek(&rbEcgMet, ECG_MET_VALID_LEN);
            ringbuffer_seek(&rbEcgMaskMet, ECG_MET_VALID_LEN);

            ecgMetResults.arrhythmiaIps = 1.0e6f / (float32_t)dwt_delta_us(tickStart);
            ecgMetResults.arrhythmiaIpspw = 1.0e3f * ecgMetResults.arrhythmiaIps / AVG_INFERENCE_POWER;

            send_ecg_metrics();
#if EN_APP_TIMING_LOGS
            nsx_printf("[ecg] metrics err=%lu\n", (unsigned long)err);
#endif
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        send_ecg_signals();
        (void)err;
    }
}

///////////////////////////////////////////////////////////////////////////////
// PPG process task
///////////////////////////////////////////////////////////////////////////////
//
// Single-wavelength pipeline (see store.h): downsample -> metrics
// (PR/QoS via pk_ppg; spo2 explicitly n/a) -> TileIO TX. No denoise/
// segmentation stage, matching legacy's pass-through behavior absent a
// second wavelength/AI model for PPG.

void
PpgProcessTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t err;

    while (true) {
        size_t numSamples = ringbuffer_len(&rbPpg1Sensor);
        for (size_t i = 0; i < numSamples / PPG_DS_RATE; i++) {
            float32_t sample;
            ringbuffer_seek(&rbPpg1Sensor, PPG_DS_RATE - 1);
            ringbuffer_peek(&rbPpg1Sensor, &sample, 1);
            ringbuffer_push(&rbPpg1Met, &sample, 1);
            ringbuffer_push(&rbPpg1Tx, &sample, 1);
            ringbuffer_seek(&rbPpg1Sensor, 1);
        }

        if (ringbuffer_len(&rbPpg1Met) >= PPG_MET_WINDOW_LEN) {
            ringbuffer_peek(&rbPpg1Met, ppg1MetData, PPG_MET_WINDOW_LEN);

            err = metrics_capture_ppg(&metricsCfg, ppg1MetData, ppg1MetData, PPG_MET_WINDOW_LEN, NULL, &ppgMetResults);
            ppgMetResults.spo2 = 0.0f; /* n/a: single-wavelength profile, see store.h */

            ringbuffer_seek(&rbPpg1Met, PPG_MET_VALID_LEN);
            send_ppg_metrics();
#if EN_APP_TIMING_LOGS
            if (err != 0) {
                nsx_printf("[ppg] metrics err=%lu\n", (unsigned long)err);
            }
#endif
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        send_ppg_signals();
    }
}

///////////////////////////////////////////////////////////////////////////////
// CPU utilization task
///////////////////////////////////////////////////////////////////////////////
//
// Reads FreeRTOS per-task run-time counters (configGENERATE_RUN_TIME_STATS,
// FreeRTOSConfig.h) to compute ECG/PPG task CPU utilization, a 30s rolling
// overall utilization average, and a battery-life estimate -- ported from
// legacy's CpuProcessTask.

void
CpuProcessTask(void *pvParameters)
{
    (void)pvParameters;
    const uint32_t samplesPerPublish = kCpuStatsPublishPeriodMs / kCpuStatsSamplePeriodMs;
    float32_t cpuUtilSecondAccum = 0.0f;
    uint32_t cpuUtilSecondCount = 0;
    float32_t cpuUtilRolling[kCpuStatsRollingSeconds] = {0};
    float32_t cpuUtilRollingSum = 0.0f;
    uint32_t cpuUtilRollingCount = 0;
    uint32_t cpuUtilRollingIndex = 0;
    uint32_t runTimeTicks = 0;
    float32_t ecgTaskPerc = 0, ppgTaskPerc = 0, totalTaskPerc = 0;
    uint32_t prevRun = 0, prevEcg = 0, prevPpg = 0, prevIdle = 0;
    uint32_t runDelta, ecgDelta, ppgDelta, idleDelta;
    size_t numTasks;

    while (true) {
        uint32_t idleCounter = 0;
        numTasks = uxTaskGetSystemState(xTaskDetails, 10, &runTimeTicks);
        runDelta = runTimeTicks - prevRun;
        if (prevRun == 0 || runDelta == 0) {
            prevRun = runTimeTicks;
            prevIdle = 0;
            for (size_t i = 0; i < numTasks; i++) {
                if (xTaskDetails[i].xHandle == ecgProcessTaskHandle) {
                    prevEcg = xTaskDetails[i].ulRunTimeCounter;
                } else if (xTaskDetails[i].xHandle == ppgProcessTaskHandle) {
                    prevPpg = xTaskDetails[i].ulRunTimeCounter;
                }
                if (xTaskDetails[i].uxCurrentPriority == tskIDLE_PRIORITY) {
                    prevIdle += xTaskDetails[i].ulRunTimeCounter;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        prevRun = runTimeTicks;

        ecgDelta = 0;
        ppgDelta = 0;
        ecgTaskPerc = 0;
        ppgTaskPerc = 0;
        for (size_t i = 0; i < numTasks; i++) {
            if (xTaskDetails[i].xHandle == ecgProcessTaskHandle) {
                ecgDelta = xTaskDetails[i].ulRunTimeCounter - prevEcg;
                prevEcg = xTaskDetails[i].ulRunTimeCounter;
                ecgTaskPerc = 100.0f * (float32_t)ecgDelta / (float32_t)runDelta;
            } else if (xTaskDetails[i].xHandle == ppgProcessTaskHandle) {
                ppgDelta = xTaskDetails[i].ulRunTimeCounter - prevPpg;
                prevPpg = xTaskDetails[i].ulRunTimeCounter;
                ppgTaskPerc = 100.0f * (float32_t)ppgDelta / (float32_t)runDelta;
            }
            if (xTaskDetails[i].uxCurrentPriority == tskIDLE_PRIORITY) {
                idleCounter += xTaskDetails[i].ulRunTimeCounter;
            }
        }

        idleDelta = idleCounter - prevIdle;
        prevIdle = idleCounter;
        uint32_t cpuIdlePerc = (runDelta > 0) ? (uint32_t)(100.0f * (float32_t)idleDelta / (float32_t)runDelta) : 0;
        if (cpuIdlePerc > 100) {
            cpuIdlePerc = 100;
        }
        float32_t cpuUtilInstant = 100.0f - (float32_t)cpuIdlePerc;
        cpuUtilSecondAccum += cpuUtilInstant;
        cpuUtilSecondCount++;
        totalTaskPerc = ecgTaskPerc + ppgTaskPerc;

        if (cpuUtilSecondCount >= samplesPerPublish) {
            float32_t cpuUtilSecondAvg = cpuUtilSecondAccum / (float32_t)cpuUtilSecondCount;
            cpuUtilSecondAccum = 0.0f;
            cpuUtilSecondCount = 0;

            if (cpuUtilRollingCount < kCpuStatsRollingSeconds) {
                cpuUtilRolling[cpuUtilRollingIndex] = cpuUtilSecondAvg;
                cpuUtilRollingSum += cpuUtilSecondAvg;
                cpuUtilRollingCount++;
            } else {
                cpuUtilRollingSum -= cpuUtilRolling[cpuUtilRollingIndex];
                cpuUtilRolling[cpuUtilRollingIndex] = cpuUtilSecondAvg;
                cpuUtilRollingSum += cpuUtilSecondAvg;
            }
            cpuUtilRollingIndex = (cpuUtilRollingIndex + 1) % kCpuStatsRollingSeconds;

            appMetResults.cpuPercUtil = cpuUtilRollingSum / (float32_t)cpuUtilRollingCount;
            float32_t avgPower =
                (appMetResults.cpuPercUtil * AVG_INFERENCE_POWER + (100.0f - appMetResults.cpuPercUtil) * AVG_SLEEP_POWER) /
                100.0f;
            appMetResults.batteryDays = BATT_POWER_CAP / avgPower / 24.0f;

            send_cpu_metrics();
        }
        appMetResults.avgAiIps = (ecgMetResults.denoiseIps + ecgMetResults.segmentIps + ecgMetResults.arrhythmiaIps) / 3.0f;

        ringbuffer_push(&rbEcgCpuTx, &ecgTaskPerc, 1);
        ringbuffer_push(&rbPpgCpuTx, &ppgTaskPerc, 1);
        ringbuffer_push(&rbTotalCpuTx, &totalTaskPerc, 1);

        send_cpu_signals();

        vTaskDelay(pdMS_TO_TICKS(kCpuStatsSamplePeriodMs));
    }
}

///////////////////////////////////////////////////////////////////////////////
// TileIO TX queue drain task
///////////////////////////////////////////////////////////////////////////////

void
TioProcessTask(void *pvParameters)
{
    (void)pvParameters;
    uint8_t packet[TIO_USB_PACKET_LEN];
    while (true) {
        check_tio_state();
        if (g_tioTxQueue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        if (!g_tio_available || !tio_usb_tx_available()) {
            vTaskDelay(kTioTxTaskPollTicks);
            continue;
        }
        if (xQueueReceive(g_tioTxQueue, packet, kTioTxTaskPollTicks) == pdTRUE) {
            tio_usb_send_slot_packet(packet, TIO_USB_PACKET_LEN);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// Report task (debug: sensor throughput + latest metrics, once/sec)
///////////////////////////////////////////////////////////////////////////////

void
ReportTask(void *pvParameters)
{
    (void)pvParameters;
    while (true) {
#if EN_APP_DEBUG_LOGS
        nsx_printf("[sensor] isr=%lu missed=%lu ppg(push=%lu drop=%lu) ecg(push=%lu drop=%lu) tio_drops=%lu\n",
                   (unsigned long)sensor_get_as7058_int_isr_count(), (unsigned long)sensor_get_irq_notify_missed_count(),
                   (unsigned long)sensor_get_ppg_push_count(), (unsigned long)sensor_get_ppg_drop_count(),
                   (unsigned long)sensor_get_ecg_push_count(), (unsigned long)sensor_get_ecg_drop_count(),
                   (unsigned long)g_tio_tx_queue_drops);
#endif
        nsx_printf("[ecg] hr=%d.%02d bpm hrv=%d.%02d ms rhythm=%d qos=%d.%02d\n", (int)ecgMetResults.hr,
                   (int)(fabsf(ecgMetResults.hr - (int)ecgMetResults.hr) * 100), (int)ecgMetResults.hrv,
                   (int)(fabsf(ecgMetResults.hrv - (int)ecgMetResults.hrv) * 100), (int)ecgMetResults.arrhythmiaLabel,
                   (int)ecgMetResults.qos, (int)(fabsf(ecgMetResults.qos - (int)ecgMetResults.qos) * 100));
        nsx_printf("[ppg] pr=%d.%02d bpm spo2=n/a qos=%d.%02d\n", (int)ppgMetResults.pr,
                   (int)(fabsf(ppgMetResults.pr - (int)ppgMetResults.pr) * 100), (int)ppgMetResults.qos,
                   (int)(fabsf(ppgMetResults.qos - (int)ppgMetResults.qos) * 100));

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int
main(void)
{
    const uint32_t tioTxQueueDepth = 32;

    nsx_core_config_t core_cfg = {
        .api = &nsx_core_V1_0_0,
    };
    NSX_TRY(nsx_core_init(&core_cfg), "Core Init failed.\n");

    sensorCtx.inputSource = appState.inputSource;
    nsxPwrCfg.perf_mode = appState.speedMode ? NSX_POWER_PERF_HIGH : NSX_POWER_PERF_LOW;

    /* Enable ITM/SWO BEFORE nsx_power_configure()/perf-mode switch -- see
     * phase-3 note in git history: enabling SWO requires briefly powering
     * up Crypto to unlock the DCU, and that handshake requests HFRC from
     * the clock manager, which hangs on Apollo5-family secure parts if the
     * CPU has already moved to a SYSPLL-sourced high-performance clock. */
    nsx_itm_printf_enable();

    NSX_TRY(nsx_power_configure(&nsxPwrCfg) != NSX_STATUS_SUCCESS, "Power Init failed.\n");
    nsx_delay_us(200000);

#if AS7058_USE_SPI
    NSX_TRY(nsx_spi_interface_init(&nsxSpiCfg, AM_HAL_IOM_2MHZ, AM_HAL_IOM_SPI_MODE_2) != NSX_STATUS_SUCCESS,
            "SPI Init Failed\n");
#else
    NSX_TRY(nsx_i2c_interface_init(&nsxI2cCfg, AS7058_I2C_SPEED_HZ) != NSX_STATUS_SUCCESS, "I2C Init Failed\n");
#endif

    NSX_TRY(rtos_time_init(), "RTOS Timer Init failed.\n");

    NSX_TRY(sensor_init(&sensorCtx) != ERR_SUCCESS, "Sensor Init failed.\n");

    NSX_TRY(tflm_init(), "TFLM Init Failed\n");
    NSX_TRY(ecg_denoise_init(), "ECG Denoise Init Failed\n");
    NSX_TRY(ecg_segmentation_init(), "ECG Segmentation Init Failed\n");
    NSX_TRY(ecg_arrhythmia_init(), "ECG Arrhythmia Init Failed\n");
    NSX_TRY(metrics_init(&metricsCfg), "Metrics Init Failed\n");

    dwt_init();

    g_tioTxQueue = xQueueCreate(tioTxQueueDepth, TIO_USB_PACKET_LEN);
    NSX_TRY((g_tioTxQueue == NULL), "TIO TX queue create failed\n");

    NSX_TRY(tio_usb_init(&tioUsbCtx) != NSX_STATUS_SUCCESS, "TileIO USB Init failed.\n");

    NSX_TRY(sensor_configure() != ERR_SUCCESS, "Sensor Configure failed.\n");

    NSX_TRY((xTaskCreate(SensorIrqTask, "SensorIrqTask", AS7058_SENSOR_TASK_STACK_WORDS, 0, AS7058_SENSOR_TASK_PRIORITY,
                          &sensorIrqTaskHandle) != pdPASS),
            "SensorIrqTask create failed.\n");
    sensor_set_irq_task_handle(sensorIrqTaskHandle);
    NSX_TRY(sensor_start() != ERR_SUCCESS, "Sensor Start failed.\n");

    NSX_TRY((xTaskCreate(EcgProcessTask, "EcgProcessTask", 2048, 0, 1, &ecgProcessTaskHandle) != pdPASS),
            "EcgProcessTask create failed.\n");
    NSX_TRY((xTaskCreate(PpgProcessTask, "PpgProcessTask", 2048, 0, 1, &ppgProcessTaskHandle) != pdPASS),
            "PpgProcessTask create failed.\n");
    NSX_TRY((xTaskCreate(CpuProcessTask, "CpuProcessTask", 1024, 0, 1, &cpuProcessTaskHandle) != pdPASS),
            "CpuProcessTask create failed.\n");
    NSX_TRY((xTaskCreate(TioProcessTask, "TioProcessTask", 1024, 0, 1, &tioProcessTaskHandle) != pdPASS),
            "TioProcessTask create failed.\n");
    NSX_TRY((xTaskCreate(ReportTask, "ReportTask", 1024, 0, 1, &reportTaskHandle) != pdPASS),
            "ReportTask create failed.\n");

    nsx_printf("heartkit-vitals-demo (nsx-port): AS7058 sensing + physiokit/heliaRT DSP+AI pipeline + TileIO USB streaming\n");

    nsx_freertos_start();

    while (1) {}
}

/* configUSE_MALLOC_FAILED_HOOK == 1 requires this application hook. */
void
vApplicationMallocFailedHook(void)
{
    nsx_printf("nsx-port: malloc failed\r\n");
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* configCHECK_FOR_STACK_OVERFLOW != 0 requires this application hook. */
void
vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    nsx_printf("nsx-port: stack overflow in %s\r\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}
