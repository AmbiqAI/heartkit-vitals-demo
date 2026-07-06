/**
 * @file main.cc
 * @brief Phase 3/4: AS7058 PPG+ECG sensing + physiokit DSP metrics, plus
 * heliaRT/TFLM AI model bring-up, on apollo510_evb.
 *
 * Ported from legacy heartkit-vitals-demo. Adds the DSP-only ECG pipeline
 * (biquad denoise -> pk_ecg peak-based segmentation -> HR/HRV metrics
 * -> simple threshold arrhythmia label) and PPG pipeline (pulse-rate +
 * quality-of-signal via pk_ppg), both driven entirely by nsx-physiokit +
 * helia-dsp (CMSISDSP). See store.h for why PPG SpO2 is not yet
 * meaningful on this sensor profile.
 *
 * Also brings up the 3 legacy TFLM ECG models (denoise/segmentation/
 * arrhythmia) via nsx-helia-rt: AiModelDemoTask loads all 3 models
 * (reusing the *_flatbuffer.h model data unchanged) and runs one timed
 * inference pass per model with synthetic input, mirroring the
 * kws_infer example's single-shot demo pattern. This validates that
 * heliaRT builds/links/runs on real hardware, and reports per-model
 * inference latency, ahead of wiring the models into the live sensor
 * pipeline as a selectable AI mode (that mode-switching orchestration
 * -- app_state_t, runtime BLE/USB control -- is ported in a later phase
 * alongside the rest of main.cc's orchestration logic).
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "am_mcu_apollo.h"

#include "nsx_core.h"
#include "nsx_freertos.h"
#include "nsx_i2c.h"
#include "nsx_power.h"
#include "nsx_spi.h"

#include "pk_ecg.h"
#include "pk_filter.h"
#include "pk_ppg.h"

#include "constants.h"
#include "metrics.h"
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
static TaskHandle_t reportTaskHandle;
static TaskHandle_t aiModelDemoTaskHandle;
static TaskHandle_t tioTxTaskHandle;

// DWT cycle counter helpers (Cortex-M55), matching the kws_infer example's
// approach to measuring per-model inference latency.
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

void
SensorIrqTask(void *pvParameters)
{
    (void)pvParameters;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        sensor_process_irq_events();
    }
}

/* DSP-only ECG pipeline: downsample -> biquad bandpass denoise ->
 * pk_ecg_find_peaks_f32 QRS segmentation -> metrics_capture_ecg (HR/HRV) ->
 * simple HR-threshold arrhythmia label. Mirrors the legacy app's
 * DenoiseModeDsp/SegmentationModeDsp/ArrhythmiaModeDsp code paths (main.cc
 * EcgProcessTask), which is the only mode not requiring an AI/TFLM model. */
void
EcgProcessTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t err;
    uint32_t numPeaks;
    size_t numSamples;

    while (true) {
        /* ECG preprocessing: downsample sensor rate (200Hz) to target rate (100Hz). */
        numSamples = ringbuffer_len(&rbEcgSensor);
        for (size_t i = 0; i < numSamples / ECG_DS_RATE; i++) {
            ringbuffer_seek(&rbEcgSensor, ECG_DS_RATE - 1);
            ringbuffer_transfer(&rbEcgSensor, &rbEcgDen, 1);
        }

        /* Denoise: biquad bandpass filtfilt over a windowed block. */
        if (ringbuffer_len(&rbEcgDen) >= ECG_DEN_WINDOW_LEN) {
            ringbuffer_peek(&rbEcgDen, ecgDenInout, ECG_DEN_WINDOW_LEN);
            err = pk_apply_biquad_filtfilt_f32(&ecgFilterCtx, ecgDenInout, ecgDenInout, ECG_DEN_WINDOW_LEN,
                                                ecgDenScratch);
            ringbuffer_push(&rbEcgSeg, &ecgDenInout[ECG_DEN_PAD_LEN], ECG_DEN_VALID_LEN);
            ringbuffer_seek(&rbEcgDen, ECG_DEN_VALID_LEN);
#if EN_APP_TIMING_LOGS
            if (err != 0) {
                nsx_printf("[ecg] denoise err=%lu\n", (unsigned long)err);
            }
#endif
        }
        /* Segmentation: DSP QRS peak detection (pk_ecg_find_peaks_f32), no AI model. */
        else if (ringbuffer_len(&rbEcgSeg) >= ECG_SEG_WINDOW_LEN) {
            ringbuffer_peek(&rbEcgSeg, ecgSegInout, ECG_SEG_WINDOW_LEN);

            numPeaks = pk_ecg_find_peaks_f32(&ecgPkPeakCtx, ecgSegInout, ECG_SEG_WINDOW_LEN, peaksMetrics, ecgSegMask);
            for (size_t i = 0; i < ECG_SEG_WINDOW_LEN; i++) {
                ecgSegMask[i] = ecgSegMask[i] > 0 ? ECG_SEG_QRS : ECG_SEG_NONE;
            }
            for (size_t i = 0; i < numPeaks; i++) {
                ecgSegMask[peaksMetrics[i]] |= (ECG_FID_PEAK_QRS << ECG_MASK_FID_PEAK_OFFSET);
            }

            ringbuffer_push(&rbEcgMet, &ecgSegInout[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);
            ringbuffer_push(&rbEcgMaskMet, &ecgSegMask[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);
            /* Tee the same denoised+masked window into the TileIO TX taps
             * (see store.h) for TioTxTask to stream live to a host
             * dashboard. */
            ringbuffer_push(&rbEcgTx, &ecgSegInout[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);
            ringbuffer_push(&rbEcgMaskTx, &ecgSegMask[ECG_SEG_PAD_LEN], ECG_SEG_VALID_LEN);
            ringbuffer_seek(&rbEcgSeg, ECG_SEG_VALID_LEN);
        }
        /* Metrics: HR + HRV via metrics_capture_ecg(), then a simple
         * HR-threshold arrhythmia label (DSP fallback -- no AI model). */
        else if (MIN(ringbuffer_len(&rbEcgMet), ringbuffer_len(&rbEcgMaskMet)) >= ECG_MET_WINDOW_LEN) {
            ringbuffer_peek(&rbEcgMet, ecgMetData, ECG_MET_WINDOW_LEN);
            ringbuffer_peek(&rbEcgMaskMet, ecgMaskMetData, ECG_MET_WINDOW_LEN);

            err = metrics_capture_ecg(&metricsCfg, ecgMetData, ecgMaskMetData, ECG_MET_WINDOW_LEN, &ecgMetResults);
            ecgMetResults.arrhythmiaLabel =
                ecgMetResults.hr < 40 ? ECG_ARR_SB : ecgMetResults.hr > 100 ? ECG_ARR_GSVT : ECG_ARR_SR;

            ringbuffer_seek(&rbEcgMet, ECG_MET_VALID_LEN);
            ringbuffer_seek(&rbEcgMaskMet, ECG_MET_VALID_LEN);
#if EN_APP_TIMING_LOGS
            if (err != 0) {
                nsx_printf("[ecg] metrics err=%lu\n", (unsigned long)err);
            }
#endif
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* PPG pipeline: pulse-rate + quality-of-signal via pk_ppg. No denoise/
 * segmentation stage is needed since the currently-applied AS7058
 * "click_ppg_ecg" profile streams only one PPG wavelength (see store.h) --
 * raw samples are transferred straight into the metrics window, matching
 * the legacy app's behavior where those stages were pass-through no-ops
 * absent a second wavelength/AI model. */
void
PpgProcessTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t err;
    size_t numSamples;

    while (true) {
        numSamples = ringbuffer_len(&rbPpg1Sensor);
        for (size_t i = 0; i < numSamples / PPG_DS_RATE; i++) {
            float32_t sample;
            ringbuffer_seek(&rbPpg1Sensor, PPG_DS_RATE - 1);
            ringbuffer_peek(&rbPpg1Sensor, &sample, 1);
            ringbuffer_push(&rbPpg1Met, &sample, 1);
            /* Tee the same downsampled sample into the TileIO TX tap (see
             * store.h) for TioTxTask to stream live to a host dashboard. */
            ringbuffer_push(&rbPpg1Tx, &sample, 1);
            ringbuffer_seek(&rbPpg1Sensor, 1);
        }

        if (ringbuffer_len(&rbPpg1Met) >= PPG_MET_WINDOW_LEN) {
            ringbuffer_peek(&rbPpg1Met, ppg1MetData, PPG_MET_WINDOW_LEN);

            /* Single wavelength: pass ppg1 for both channels. AC/DC ratio
             * (and therefore spo2) is meaningless with one wavelength, so
             * it is discarded below; pr/qos remain valid. */
            err = metrics_capture_ppg(&metricsCfg, ppg1MetData, ppg1MetData, PPG_MET_WINDOW_LEN, NULL, &ppgMetResults);
            ppgMetResults.spo2 = 0.0f; /* n/a: single-wavelength profile, see store.h */

            ringbuffer_seek(&rbPpg1Met, PPG_MET_VALID_LEN);
#if EN_APP_TIMING_LOGS
            if (err != 0) {
                nsx_printf("[ppg] metrics err=%lu\n", (unsigned long)err);
            }
#endif
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* Brings up the 3 legacy TFLM ECG models (denoise/segmentation/
 * arrhythmia) via nsx-helia-rt and runs one timed inference pass per
 * model with a synthetic sine-wave input, reporting arena usage and
 * latency. This is a bring-up/validation task (mirrors the kws_infer
 * example's single-shot demo pattern) -- it does not yet feed live
 * sensor data or replace the DSP pipeline above; that full AI-mode
 * wiring (mode switching, live windowed staging matching each model's
 * pad length) is deferred to the main-orchestration porting phase. */
void
AiModelDemoTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t err;
    uint32_t cyclesStart, cyclesEnd;
    static float32_t synthEcgDen[ECG_DEN_WINDOW_LEN];
    static float32_t synthEcgDenOut[ECG_DEN_WINDOW_LEN];
    static float32_t synthEcgSeg[ECG_SEG_WINDOW_LEN];
    static uint16_t synthSegMask[ECG_SEG_WINDOW_LEN];
    static float32_t synthEcgArr[ECG_ARR_WINDOW_LEN];
    float32_t qos = 0;

    dwt_init();

    for (size_t i = 0; i < ECG_DEN_WINDOW_LEN; i++) {
        synthEcgDen[i] = sinf(2.0f * (float32_t)M_PI * 1.2f * i / ECG_TARGET_RATE);
    }
    for (size_t i = 0; i < ECG_SEG_WINDOW_LEN; i++) {
        synthEcgSeg[i] = sinf(2.0f * (float32_t)M_PI * 1.2f * i / ECG_TARGET_RATE);
    }
    for (size_t i = 0; i < ECG_ARR_WINDOW_LEN; i++) {
        synthEcgArr[i] = sinf(2.0f * (float32_t)M_PI * 1.2f * i / ECG_TARGET_RATE);
    }

    nsx_printf("[ai] initializing TFLM (heliaRT) backend...\n");
    tflm_init();

    err = ecg_denoise_init();
    nsx_printf("[ai] ecg_denoise_init err=%lu\n", (unsigned long)err);

    err = ecg_segmentation_init();
    nsx_printf("[ai] ecg_segmentation_init err=%lu\n", (unsigned long)err);

    err = ecg_arrhythmia_init();
    nsx_printf("[ai] ecg_arrhythmia_init err=%lu\n", (unsigned long)err);

    while (true) {
        cyclesStart = dwt_cycles();
        err = ecg_denoise_inference(synthEcgDen, synthEcgDenOut, ECG_DEN_PAD_LEN, ECG_DEN_THRESHOLD);
        cyclesEnd = dwt_cycles();
        nsx_printf("[ai] denoise inference err=%lu latency_us=%lu\n", (unsigned long)err,
                   (unsigned long)((cyclesEnd - cyclesStart) / (SystemCoreClock / 1000000)));

        cyclesStart = dwt_cycles();
        err = ecg_segmentation_inference(synthEcgSeg, synthSegMask, ECG_SEG_PAD_LEN, ECG_SEG_THRESHOLD, &qos);
        cyclesEnd = dwt_cycles();
        nsx_printf("[ai] segmentation inference err=%lu qos=%d latency_us=%lu\n", (unsigned long)err, (int)qos,
                   (unsigned long)((cyclesEnd - cyclesStart) / (SystemCoreClock / 1000000)));

        cyclesStart = dwt_cycles();
        uint32_t arrLabel = ecg_arrhythmia_inference(synthEcgArr, ECG_ARR_THRESHOLD);
        cyclesEnd = dwt_cycles();
        nsx_printf("[ai] arrhythmia inference label=%lu latency_us=%lu\n", (unsigned long)arrLabel,
                   (unsigned long)((cyclesEnd - cyclesStart) / (SystemCoreClock / 1000000)));

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

///////////////////////////////////////////////////////////////////////////////
// TileIO USB streaming
///////////////////////////////////////////////////////////////////////////////
//
// Ported from legacy's tio_usb.h/webusb_controller.h usage onto
// nsx-tileio-usb (a thin wrapper carrying the same TileIO packet framing
// over the public nsx-usb vendor-channel API -- see
// modules/nsx-tileio/modules/nsx-tileio-usb/README.md). Streams: slot 0 =
// ECG (type 0 = denoised+mask signal, type 1 = HR/HRV/QoS/arrhythmia
// metrics), slot 1 = PPG (type 0 = single-wavelength signal, type 1 =
// PR/QoS metrics -- SpO2 is n/a, see store.h). UIO (input source, noise
// levels, denoise/segmentation/arrhythmia mode selectors) receive is
// stubbed/logged only -- there is no app_state_t-equivalent runtime mode
// switch in nsx-port yet to actually apply those settings (that lands in
// the main-orchestration porting phase); this still proves the TileIO USB
// transport itself (enumeration, framing, RX/TX) end-to-end on hardware.

// Forward declarations (defined below tioUsbCtx, referenced by it).
static void TioUioUpdate(const uint8_t *data, uint32_t length);
static void TioSlotUpdate(uint8_t slot, uint8_t slot_type, const uint8_t *data, uint32_t length);

static tio_usb_context_t tioUsbCtx = {
    .uio_update_cb = &TioUioUpdate,
    .slot_update_cb = &TioSlotUpdate,
    .manufacturer = "Ambiq",
    .product = "heartkit-vitals-demo (nsx-port)",
    .serial = "NSX-HKV-0001",
    .cdc_interface = "NSX CDC",
    .vendor_interface = "TileIO Vendor",
    .webusb_url = "tileio.local",
    .vid = TIO_USB_VENDOR_ID,
    .pid = TIO_USB_PRODUCT_ID,
};

static void
TioUioUpdate(const uint8_t *data, uint32_t length)
{
    nsx_printf("[tio] uio update received (len=%lu) -- mode switching not yet wired in nsx-port\n",
               (unsigned long)length);
    (void)data;
}

static void
TioSlotUpdate(uint8_t slot, uint8_t slot_type, const uint8_t *data, uint32_t length)
{
    nsx_printf("[tio] slot update received slot=%d type=%d len=%lu (no host->device slot data expected)\n",
               (int)slot, (int)slot_type, (unsigned long)length);
    (void)data;
}

/* Packs+sends the denoised ECG (2ch: value, QRS mask) from rbEcgTx/
 * rbEcgMaskTx, PPG (1ch: single-wavelength value) from rbPpg1Tx, and the
 * latest ECG/PPG metrics snapshots -- to the TileIO host over USB. */
void
TioTxTask(void *pvParameters)
{
    (void)pvParameters;
    uint8_t buffer[240];
    float32_t ecgVal, ppgVal;
    uint16_t ecgMaskVal;
    int16_t txVal;
    uint32_t length;
    size_t numSamples;
    float32_t metricsBuffer[16];

    while (true) {
        if (!tio_usb_tx_available()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* ECG signal (slot 0, type 0): interleaved mask + denoised value. */
        numSamples = MIN(ringbuffer_len(&rbEcgTx), ringbuffer_len(&rbEcgMaskTx));
        numSamples = MIN(numSamples, sizeof(buffer) / (sizeof(uint16_t) + sizeof(int16_t)));
        if (numSamples > 0) {
            length = 0;
            for (size_t i = 0; i < numSamples; i++) {
                ringbuffer_pop(&rbEcgMaskTx, &ecgMaskVal, 1);
                memcpy(&buffer[length], &ecgMaskVal, sizeof(uint16_t));
                length += sizeof(uint16_t);
                ringbuffer_pop(&rbEcgTx, &ecgVal, 1);
                txVal = (int16_t)CLIP(TIO_SLOT0_SCALE * ecgVal, -32768, 32767);
                memcpy(&buffer[length], &txVal, sizeof(int16_t));
                length += sizeof(int16_t);
            }
            tio_usb_send_slot_data(0, 0, buffer, length);
        }

        /* PPG signal (slot 1, type 0): single wavelength value only (see
         * store.h for why there is no second channel to send). */
        numSamples = ringbuffer_len(&rbPpg1Tx);
        numSamples = MIN(numSamples, sizeof(buffer) / sizeof(int16_t));
        if (numSamples > 0) {
            length = 0;
            for (size_t i = 0; i < numSamples; i++) {
                ringbuffer_pop(&rbPpg1Tx, &ppgVal, 1);
                txVal = (int16_t)CLIP(ppgVal, -32768.0f, 32767.0f);
                memcpy(&buffer[length], &txVal, sizeof(int16_t));
                length += sizeof(int16_t);
            }
            tio_usb_send_slot_data(1, 0, buffer, length);
        }

        /* ECG metrics (slot 0, type 1). */
        metricsBuffer[0] = ecgMetResults.hr;
        metricsBuffer[1] = ecgMetResults.hrv;
        metricsBuffer[2] = ecgMetResults.arrhythmiaLabel;
        metricsBuffer[3] = ecgMetResults.qos;
        tio_usb_send_slot_data(0, 1, (const uint8_t *)metricsBuffer, 4 * sizeof(float32_t));

        /* PPG metrics (slot 1, type 1). spo2 is always 0 -- n/a (see store.h). */
        metricsBuffer[0] = ppgMetResults.pr;
        metricsBuffer[1] = ppgMetResults.spo2;
        metricsBuffer[2] = ppgMetResults.qos;
        tio_usb_send_slot_data(1, 1, (const uint8_t *)metricsBuffer, 3 * sizeof(float32_t));

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Reports sensor throughput plus the latest DSP metrics every second. */
void
ReportTask(void *pvParameters)
{
    (void)pvParameters;
    while (true) {
        nsx_printf("[sensor] isr=%lu missed=%lu ppg(push=%lu drop=%lu) ecg(push=%lu drop=%lu)\n",
                   (unsigned long)sensor_get_as7058_int_isr_count(),
                   (unsigned long)sensor_get_irq_notify_missed_count(),
                   (unsigned long)sensor_get_ppg_push_count(), (unsigned long)sensor_get_ppg_drop_count(),
                   (unsigned long)sensor_get_ecg_push_count(), (unsigned long)sensor_get_ecg_drop_count());
        nsx_printf("[ecg] hr=%d.%02d bpm hrv=%d.%02d ms rhythm=%d qos=%d.%02d\n",
                   (int)ecgMetResults.hr, (int)(fabsf(ecgMetResults.hr - (int)ecgMetResults.hr) * 100),
                   (int)ecgMetResults.hrv, (int)(fabsf(ecgMetResults.hrv - (int)ecgMetResults.hrv) * 100),
                   (int)ecgMetResults.arrhythmiaLabel,
                   (int)ecgMetResults.qos, (int)(fabsf(ecgMetResults.qos - (int)ecgMetResults.qos) * 100));
        nsx_printf("[ppg] pr=%d.%02d bpm spo2=n/a qos=%d.%02d\n",
                   (int)ppgMetResults.pr, (int)(fabsf(ppgMetResults.pr - (int)ppgMetResults.pr) * 100),
                   (int)ppgMetResults.qos, (int)(fabsf(ppgMetResults.qos - (int)ppgMetResults.qos) * 100));

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

int
main(void)
{
    nsx_core_config_t core_cfg = {
        .api = &nsx_core_V1_0_0,
    };
    NSX_TRY(nsx_core_init(&core_cfg), "Core Init failed.\n");

    /* Enable ITM/SWO before the perf-mode switch (see ppg-codec-demo main.c
     * for the Apollo5-family DCU/HFRC ordering rationale). */
    nsx_itm_printf_enable();

    NSX_TRY(nsx_power_configure(&nsxPwrCfg) != NSX_STATUS_SUCCESS, "Power Init failed.\n");
    nsx_delay_us(200000);

#if AS7058_USE_SPI
    NSX_TRY(nsx_spi_interface_init(&nsxSpiCfg, AM_HAL_IOM_2MHZ, AM_HAL_IOM_SPI_MODE_2) != NSX_STATUS_SUCCESS,
            "SPI Init Failed\n");
#else
    NSX_TRY(nsx_i2c_interface_init(&nsxI2cCfg, AS7058_I2C_SPEED_HZ) != NSX_STATUS_SUCCESS, "I2C Init Failed\n");
#endif

    NSX_TRY(sensor_init(&sensorCtx) != ERR_SUCCESS, "Sensor Init failed.\n");
    NSX_TRY(sensor_configure() != ERR_SUCCESS, "Sensor Configure failed.\n");

    NSX_TRY(tio_usb_init(&tioUsbCtx) != NSX_STATUS_SUCCESS, "TileIO USB Init failed.\n");

    NSX_TRY((xTaskCreate(
               SensorIrqTask,
               "SensorIrqTask",
               AS7058_SENSOR_TASK_STACK_WORDS,
               0,
               AS7058_SENSOR_TASK_PRIORITY,
               &sensorIrqTaskHandle) != pdPASS),
           "SensorIrqTask create failed.\n");

    sensor_set_irq_task_handle(sensorIrqTaskHandle);
    NSX_TRY(sensor_start() != ERR_SUCCESS, "Sensor Start failed.\n");

    NSX_TRY((xTaskCreate(
               EcgProcessTask,
               "EcgProcessTask",
               2048,
               0,
               1,
               &ecgProcessTaskHandle) != pdPASS),
           "EcgProcessTask create failed.\n");

    NSX_TRY((xTaskCreate(
               PpgProcessTask,
               "PpgProcessTask",
               2048,
               0,
               1,
               &ppgProcessTaskHandle) != pdPASS),
           "PpgProcessTask create failed.\n");

    NSX_TRY((xTaskCreate(
               ReportTask,
               "ReportTask",
               1024,
               0,
               1,
               &reportTaskHandle) != pdPASS),
           "ReportTask create failed.\n");

    NSX_TRY((xTaskCreate(
               AiModelDemoTask,
               "AiModelDemoTask",
               4096,
               0,
               1,
               &aiModelDemoTaskHandle) != pdPASS),
           "AiModelDemoTask create failed.\n");

    NSX_TRY((xTaskCreate(
               TioTxTask,
               "TioTxTask",
               2048,
               0,
               1,
               &tioTxTaskHandle) != pdPASS),
           "TioTxTask create failed.\n");

    nsx_printf("nsx-port phase 3/4/5: AS7058 PPG+ECG sensing + physiokit DSP metrics + heliaRT AI model bring-up + "
               "TileIO USB streaming\n");

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
