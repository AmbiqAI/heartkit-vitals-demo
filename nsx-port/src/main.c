/**
 * @file main.c
 * @brief Phase 3: AS7058 PPG+ECG sensing + physiokit DSP metrics on apollo510_evb.
 *
 * Ported from legacy heartkit-vitals-demo. Adds the DSP-only ECG pipeline
 * (biquad denoise -> pk_ecg peak-based segmentation -> HR/HRV metrics
 * -> simple threshold arrhythmia label) and PPG pipeline (pulse-rate +
 * quality-of-signal via pk_ppg), both driven entirely by nsx-physiokit +
 * helia-dsp (CMSISDSP) -- no AI/TFLM model dependency yet (that's heliaRT,
 * a later phase). See store.h for why PPG SpO2 is not yet meaningful on
 * this sensor profile.
 */
#include <math.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

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

static TaskHandle_t sensorIrqTaskHandle;
static TaskHandle_t ecgProcessTaskHandle;
static TaskHandle_t ppgProcessTaskHandle;
static TaskHandle_t reportTaskHandle;

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
            ringbuffer_seek(&rbPpg1Sensor, PPG_DS_RATE - 1);
            ringbuffer_transfer(&rbPpg1Sensor, &rbPpg1Met, 1);
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

    nsx_printf("nsx-port phase 3: AS7058 PPG+ECG sensing + physiokit DSP metrics\n");

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
