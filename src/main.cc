// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/** @file main.cc
 * @brief Vital Sign Monitoring application orchestration.
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
#include "nsx_usb.h"

#include "pk_ecg.h"
#include "pk_filter.h"
#include "pk_math.h"
#include "pk_ppg.h"

#include "constants.h"
#include "battery_model.h"
#include "inference_timing.h"
#if defined(AM_PART_APOLLO330P)
#include "am_bsp.h"
#endif
#include "metrics.h"
#include "nstdb_noise.h"
#include "obs.h"
#include "ringbuffer.h"
#include "sensor.h"
#include "sensor_bus.h"
#include "store.h"
#include "telemetry.h"
#include "timebase.h"
#include "tio_tx_sm.h"

#include "ecg_arrhythmia.h"
#include "ecg_denoise.h"
#include "ecg_segmentation.h"

#include "tio_usb.h"

/* Match the BLE source gate in the build configuration. */
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
#include "ble_bringup.h"
#endif

static TaskHandle_t sensorIrqTaskHandle;
static TaskHandle_t ecgProcessTaskHandle;
static TaskHandle_t ppgProcessTaskHandle;
static TaskHandle_t cpuProcessTaskHandle;
static TaskHandle_t tioProcessTaskHandle;
static TaskHandle_t reportTaskHandle;

/* uxTaskGetSystemState requires room for every task; an undersized snapshot fails. */
#define HKV_TASK_STATUS_CAPACITY (16u)
static TaskStatus_t xTaskDetails[HKV_TASK_STATUS_CAPACITY];
/* Tasks seen by the last successful uxTaskGetSystemState(), reported so the
 * headroom against HKV_TASK_STATUS_CAPACITY is visible before it runs out. */
static volatile uint32_t g_cpu_num_tasks = 0;

static QueueHandle_t g_tioTxQueue = NULL;
static const TickType_t kTioTxTaskPollTicks = pdMS_TO_TICKS(10);
/* Hold time after which a packet USB will not take counts as a stalled host
 * rather than a hiccup: past TIO_JITTER_BUDGET_MS the gap is visible at the
 * host. Observability only -- the packet is still retried, never dropped. */
static const uint32_t kTioUsbStallThresholdMs = TIO_JITTER_BUDGET_MS;
static const uint32_t kCpuStatsSamplePeriodMs = 100;
static const uint32_t kCpuStatsPublishPeriodMs = 1000;
static const uint32_t kCpuStatsRollingSeconds = 30;

// DWT cycle counter for stage timing.

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
    /* Correct as long as SystemCoreClock is truthful, which timebase_sync_to_
     * core_clock() keeps it (issue #25). An inference that spans a speed-mode
     * switch is scaled by whichever value is live when it FINISHES, so that
     * one measurement is wrong; the next one is right. Accepted. */
    return deltaCycles / (SystemCoreClock / 1000000);
}

/* Read the operating point once so a mode change cannot mix profile values. */
static inline float32_t
inference_power_mw(bool hpMode)
{
    return hpMode ? (float32_t)MCU_INFERENCE_POWER_MW_HP : (float32_t)MCU_INFERENCE_POWER_MW_LP;
}

// FreeRTOS runtime-statistics timer.

static volatile uint32_t g_rtos_stat_timer_cnt = 0;

extern "C" uint32_t
rtos_time_init(void)
{
    uint32_t timerNum = RTOS_TIMER;
    uint32_t status;
    g_rtos_stat_timer_cnt = 0;
    am_hal_timer_config_t rtosTimerConfig;
    am_hal_timer_default_config_set(&rtosTimerConfig);
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

// TileIO streaming and host controls.

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
    /* nsx-usb encodes WebUSB scheme 1 (HTTPS) separately, so this must be
     * the host/path only, not a full URL. */
    .webusb_url = "ambiqai.github.io/tileio/",
    .vid = TIO_USB_VENDOR_ID,
    .pid = TIO_USB_PRODUCT_ID,
};

/* Only each ring's consumer may flush its tail; request flushes in the owning task. */
static volatile uint8_t g_flush_req_ecg = 0;
static volatile uint8_t g_flush_req_ppg = 0;
static volatile uint8_t g_flush_req_cpu = 0;

static void
request_pipeline_flush(void)
{
    g_flush_req_ecg = 1;
    g_flush_req_ppg = 1;
    g_flush_req_cpu = 1;
    if (g_tioTxQueue != NULL) {
        xQueueReset(g_tioTxQueue);
    }
}

/* EcgProcessTask owns (is sole consumer of) all ECG-side buffers. */
static void
service_ecg_flush_request(void)
{
    if (!g_flush_req_ecg) {
        return;
    }
    g_flush_req_ecg = 0;
    ringbuffer_flush(&rbEcgSensor);
    ringbuffer_flush(&rbEcgDen);
    ringbuffer_flush(&rbEcgRawSeg);
    ringbuffer_flush(&rbEcgSeg);
    ringbuffer_flush(&rbEcgMet);
    ringbuffer_flush(&rbEcgMaskMet);
    ringbuffer_flush(&rbEcgRawTx);
    ringbuffer_flush(&rbEcgDenTx);
    ringbuffer_flush(&rbEcgMaskTx);
}

/* PpgProcessTask owns all PPG-side buffers. */
static void
service_ppg_flush_request(void)
{
    if (!g_flush_req_ppg) {
        return;
    }
    g_flush_req_ppg = 0;
    ringbuffer_flush(&rbPpg1Sensor);
    ringbuffer_flush(&rbPpg2Sensor);
    ringbuffer_flush(&rbPpg1Met);
    ringbuffer_flush(&rbPpg2Met);
    ringbuffer_flush(&rbPpg1Tx);
    ringbuffer_flush(&rbPpg2Tx);
}

/* CpuProcessTask owns the CPU-stat TX buffers. */
static void
service_cpu_flush_request(void)
{
    if (!g_flush_req_cpu) {
        return;
    }
    g_flush_req_cpu = 0;
    ringbuffer_flush(&rbEcgCpuTx);
    ringbuffer_flush(&rbPpgCpuTx);
    ringbuffer_flush(&rbTotalCpuTx);
}

/* Slot id / packet type byte offsets inside a packed TileIO frame. tio_usb.c
 * keeps TIO_USB_SLOT_IDX/TIO_USB_TYPE_IDX private; mirrored here so the drain
 * task can attribute a USB retry/drop to the stream that produced the packet. */
#define TIO_PACKET_SLOT_IDX 1u
#define TIO_PACKET_TYPE_IDX 2u
#define TIO_PACKET_TYPE_UIO 2u

/* USB counter buckets: 0=ECG slot, 1=PPG slot, 2=CPU slot, 3=UIO responses.
 * UIO rides slot 0 (see send_uio_state) and would otherwise be mis-attributed
 * to ECG, so it is bucketed by packet type instead. */
#define TIO_USB_BUCKET_COUNT 4u
#define TIO_USB_BUCKET_UIO 3u

/* Attribute a USB retry/drop to the stream that produced the packet. Storage
 * is the obs.h counter table (HKV_CNT_USB_*); `which` selects retry vs drop
 * within a bucket and the stride is static_asserted in obs.c. */
static void
count_tio_usb_event(uint32_t which, const uint8_t packet[TIO_USB_PACKET_LEN])
{
    uint8_t slot;
    if (packet[TIO_PACKET_TYPE_IDX] == TIO_PACKET_TYPE_UIO) {
        hkv_count(HKV_CNT_USB_BUCKET(TIO_USB_BUCKET_UIO, which));
        return;
    }
    slot = packet[TIO_PACKET_SLOT_IDX];
    if (slot < 3) {
        hkv_count(HKV_CNT_USB_BUCKET(slot, which));
    }
}


static bool
enqueue_tio_packet(const uint8_t packet[TIO_USB_PACKET_LEN])
{
    BaseType_t queued = pdFALSE;
    if (g_tioTxQueue == NULL) {
        hkv_count(HKV_CNT_TIO_QDROP);
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
        hkv_count(HKV_CNT_TIO_QDROP);
        return false;
    }
    return true;
}

/* UIO responses unblock host controls. They are only queued from task
 * context, so put them ahead of waveform backlog and discard one stale
 * packet if the bounded queue is full. */
static bool
enqueue_tio_packet_priority(const uint8_t packet[TIO_USB_PACKET_LEN])
{
    uint8_t dropped_packet[TIO_USB_PACKET_LEN];

    if (g_tioTxQueue == NULL) {
        hkv_count(HKV_CNT_TIO_QDROP);
        return false;
    }
    if (xQueueSendToFront(g_tioTxQueue, packet, 0) == pdTRUE) {
        return true;
    }
    if (xQueueReceive(g_tioTxQueue, dropped_packet, 0) != pdTRUE ||
        xQueueSendToFront(g_tioTxQueue, packet, 0) != pdTRUE) {
        hkv_count(HKV_CNT_TIO_QDROP);
        return false;
    }
    hkv_count(HKV_CNT_TIO_QDROP);
    return true;
}

static bool
pack_and_enqueue_tio_packet(uint8_t slot, uint8_t slot_type, const void *payload, uint32_t payload_len)
{
    uint8_t packet[TIO_USB_PACKET_LEN];
    bool ok;
    if (tio_usb_pack_slot_data(slot, slot_type, (const uint8_t *)payload, payload_len, packet) != 0) {
        if (slot < 3) {
            hkv_count(HKV_CNT_TIO_SLOT(slot, HKV_TIO_WHICH_PACKFAIL));
        }
        return false;
    }
    ok = enqueue_tio_packet(packet);
    if (slot < 3) {
        if (ok) {
            hkv_count(HKV_CNT_TIO_SLOT(slot, HKV_TIO_WHICH_OK));
        } else {
            hkv_count(HKV_CNT_TIO_SLOT(slot, HKV_TIO_WHICH_FAIL));
        }
    }
    return ok;
}

static bool
pack_and_enqueue_tio_packet_priority(uint8_t slot, uint8_t slot_type, const void *payload, uint32_t payload_len)
{
    uint8_t packet[TIO_USB_PACKET_LEN];

    if (tio_usb_pack_slot_data(slot, slot_type, (const uint8_t *)payload, payload_len, packet) != 0) {
        return false;
    }
    return enqueue_tio_packet_priority(packet);
}

static volatile bool g_tio_available = false;

static void send_uio_state(void);

static void
check_tio_state(void)
{
    bool available = tio_usb_tx_available() != 0;
    if (available != g_tio_available) {
        g_tio_available = available;
        if (g_tio_available) {
            hkv_log_begin("tio");
            hkv_log_u32("host_connected", 1);
            hkv_log_end();
            request_pipeline_flush();
            /* Synchronize host controls without requiring an initial state write. */
            send_uio_state();
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
        hkv_log_begin("app");
        hkv_log_u32("input_source", sensorCtx.inputSource);
        hkv_log_str("input_kind", source < NUM_INPUT_PTS ? "canned" : "live");
        hkv_log_end();
    }
}

static void
set_noise_inputs(uint8_t bw, uint8_t ma, uint8_t em)
{
    appState.bwNoiseLevel = bw;
    appState.maNoiseLevel = ma;
    appState.emNoiseLevel = em;
    hkv_log_begin("app");
    hkv_log_u32("noise_bw", appState.bwNoiseLevel);
    hkv_log_u32("noise_ma", appState.maNoiseLevel);
    hkv_log_u32("noise_em", appState.emNoiseLevel);
    hkv_log_end();
}

static void
set_denoise_mode(uint8_t mode)
{
    mode = MIN(mode, 2);
    if (appState.denoiseMode != mode) {
        appState.denoiseMode = mode;
        hkv_log_begin("app");
        hkv_log_u32("denoise_mode", appState.denoiseMode);
        hkv_log_end();
    }
}

static void
set_segmentation_mode(uint8_t mode)
{
    mode = MIN(mode, 2);
    if (appState.segMode != mode) {
        appState.segMode = mode;
        hkv_log_begin("app");
        hkv_log_u32("seg_mode", appState.segMode);
        hkv_log_end();
    }
}

static void
set_arrhythmia_mode(uint8_t mode)
{
    mode = MIN(mode, 2);
    if (appState.arrMode != mode) {
        appState.arrMode = mode;
        hkv_log_begin("app");
        hkv_log_u32("arr_mode", appState.arrMode);
        hkv_log_end();
    }
}

static void
set_speed_mode(uint8_t mode)
{
    mode = MIN(mode, 1);
    if (appState.speedMode != mode) {
        appState.speedMode = mode;
        nsx_power_set_performance_mode(appState.speedMode ? NSX_POWER_PERF_HIGH : NSX_POWER_PERF_LOW);
        /* Resynchronize tick and duration scaling after a mode change; see AmbiqAI/heartkit-vitals-demo#25. */
        timebase_sync_to_core_clock();
        /* Samples taken at the previous operating point do not describe this
         * one, and the max would otherwise carry them for a whole interval. */
        ecgMetResults.denoiseLatMaxUs = 0;
        ecgMetResults.segmentLatMaxUs = 0;
        ecgMetResults.arrhythmiaLatMaxUs = 0;
        hkv_log_begin("app");
        hkv_log_u32("speed_mode", appState.speedMode);
        hkv_log_end();
    }
}

static void
snapshot_uio_state(uint8_t uioBuffer[8])
{
    uioBuffer[TIO_UIO_INPUT_SEL_IDX] = appState.inputSource;
    uioBuffer[TIO_UIO_BW_NOISE_IDX] = appState.bwNoiseLevel;
    uioBuffer[TIO_UIO_MA_NOISE_IDX] = appState.maNoiseLevel;
    uioBuffer[TIO_UIO_EM_NOISE_IDX] = appState.emNoiseLevel;
    uioBuffer[TIO_UIO_SPEED_MODE_IDX] = appState.speedMode;
    uioBuffer[TIO_UIO_DEN_MODE_IDX] = appState.denoiseMode;
    uioBuffer[TIO_UIO_SEG_MODE_IDX] = appState.segMode;
    uioBuffer[TIO_UIO_ARR_MODE_IDX] = appState.arrMode;
}

static void
send_uio_state(void)
{
    uint8_t uioBuffer[8];
    snapshot_uio_state(uioBuffer);
    /* Replies must enqueue without blocking the callback context. */
    pack_and_enqueue_tio_packet_priority(0, 2, uioBuffer, sizeof(uioBuffer));
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

#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
/* C-linkage adapters allow the BLE module to call the application callbacks. */
extern "C" void
ble_bringup_slot_update_cb(uint8_t slot, uint8_t slot_type, const uint8_t *data, uint32_t length)
{
    received_slot_data(slot, slot_type, data, length);
}

extern "C" void
ble_bringup_uio_update_cb(const uint8_t *data, uint32_t length)
{
    received_uio_state(data, length);
}

extern "C" void
ble_bringup_uio_read_cb(uint8_t *data, uint32_t length)
{
    /* Invoked in the BLE stack's GATT read-handler context.  Keep this as a
     * bounded snapshot only: no queues, logging, mode changes, or BLE sends. */
    if (data != nullptr && length == 8u) {
        snapshot_uio_state(data);
    }
}
#endif

/* USB callbacks run in interrupt context; defer settings and replies to TioProcessTask. */
static volatile uint8_t g_uio_pending = 0;
static volatile uint8_t g_uio_state_request_pending = 0;
static uint8_t g_uio_rx_buf[8];

static void
received_uio_state(const uint8_t *data, uint32_t length)
{
    /* A zero-length UIO frame asks for the current state. It is distinct from
     * a valid all-zero eight-byte state update. */
    if (length == 0) {
        g_uio_state_request_pending = 1;
        return;
    }
    if (length != 8) {
        return;
    }
    memcpy(g_uio_rx_buf, data, 8);
    __asm volatile("" ::: "memory");
    hkv_count(HKV_CNT_TIO_UIO_RX);
    g_uio_pending = 1;
}

/* Runs in TioProcessTask context. */
static void
apply_pending_uio_state(void)
{
    uint8_t local[8];
    if (!g_uio_pending) {
        return;
    }
    /* Clear-then-copy with a re-check: if the ISR delivers a fresh UIO write
     * mid-copy (re-raising the flag), loop and re-copy so we never act on a
     * torn snapshot and never silently drop the newest state. */
    do {
        g_uio_pending = 0;
        __asm volatile("" ::: "memory");
        memcpy(local, g_uio_rx_buf, 8);
        __asm volatile("" ::: "memory");
    } while (g_uio_pending);

    set_input_source(local[TIO_UIO_INPUT_SEL_IDX]);
    set_noise_inputs(local[TIO_UIO_BW_NOISE_IDX], local[TIO_UIO_MA_NOISE_IDX], local[TIO_UIO_EM_NOISE_IDX]);
    set_speed_mode(local[TIO_UIO_SPEED_MODE_IDX]);
    set_denoise_mode(local[TIO_UIO_DEN_MODE_IDX]);
    set_segmentation_mode(local[TIO_UIO_SEG_MODE_IDX]);
    set_arrhythmia_mode(local[TIO_UIO_ARR_MODE_IDX]);
    send_uio_state();
}

///////////////////////////////////////////////////////////////////////////////
// TIO packet senders
///////////////////////////////////////////////////////////////////////////////

// Advance every ring in a signal group equally to preserve channel alignment.
// See AmbiqAI/heartkit-vitals-demo#12.

typedef struct {
    rb_config_t *rings[3];   /* parallel TX taps, must stay sample-aligned */
    uint8_t numRings;
    uint16_t highWater;      /* H, samples (constants.h) */
    uint16_t samplesPerPkt;  /* nominal packet size, samples */
    uint16_t troughTarget;   /* target steady-state trough (constants.h) */
    /* Trough servo state. See tio_tx_group_servo() for the control argument. */
    uint16_t servoTicks;         /* ticks elapsed in the current servo window */
    uint16_t extraSpent;         /* extra samples already spent this window */
    uint32_t servoTroughMin;     /* running min occupancy, current window */
    uint32_t servoTroughPrev;    /* previous window's trough, for the rate term */
    volatile uint16_t extraBudget;  /* servo output: extra samples per window */
    volatile uint32_t servoTrough;  /* last completed window's trough (log) */
    /* Pumped counts assembled packets; delivered counts accepted packets.
     * Drained counts extra samples, while occupancy is sampled before trim/pop. */
    hkv_counter_id_t cntTrimmed;   /* samples discarded by trim-to-high-water */
    hkv_counter_id_t cntPumped;
    hkv_counter_id_t cntDelivered;
    hkv_counter_id_t cntDrained;
    hkv_gauge_id_t gaugeOcc;
} tio_tx_group_t;

#define TIO_OCC_MIN_INIT (0xFFFFFFFFu)

/* Named fields prevent silent swaps between same-typed thresholds and counter IDs. */
static tio_tx_group_t g_ecgTxGroup = {
    .rings = {&rbEcgMaskTx, &rbEcgRawTx, &rbEcgDenTx},
    .numRings = 3,
    .highWater = TIO_ECG_TX_HIGH_WATER,
    .samplesPerPkt = TIO_ECG_SAMPLES_PER_PKT,
    .troughTarget = TIO_ECG_TX_TROUGH_TARGET,
    .servoTicks = 0,
    .extraSpent = 0,
    .servoTroughMin = TIO_OCC_MIN_INIT,
    .servoTroughPrev = TIO_ECG_TX_TROUGH_TARGET,
    .extraBudget = 0,
    .servoTrough = 0,
    .cntTrimmed = HKV_CNT_TXECG_TRIM,
    .cntPumped = HKV_CNT_TXECG_PUMP,
    .cntDelivered = HKV_CNT_TXECG_DELIV,
    .cntDrained = HKV_CNT_TXECG_DRAIN,
    .gaugeOcc = HKV_GAUGE_TXECG_OCC};
static tio_tx_group_t g_ppgTxGroup = {
    .rings = {&rbPpg1Tx, &rbPpg2Tx, NULL},
    .numRings = 2,
    .highWater = TIO_PPG_TX_HIGH_WATER,
    .samplesPerPkt = TIO_PPG_SAMPLES_PER_PKT,
    .troughTarget = TIO_PPG_TX_TROUGH_TARGET,
    .servoTicks = 0,
    .extraSpent = 0,
    .servoTroughMin = TIO_OCC_MIN_INIT,
    .servoTroughPrev = TIO_PPG_TX_TROUGH_TARGET,
    .extraBudget = 0,
    .servoTrough = 0,
    .cntTrimmed = HKV_CNT_TXPPG_TRIM,
    .cntPumped = HKV_CNT_TXPPG_PUMP,
    .cntDelivered = HKV_CNT_TXPPG_DELIV,
    .cntDrained = HKV_CNT_TXPPG_DRAIN,
    .gaugeOcc = HKV_GAUGE_TXPPG_OCC};

/* Reserve room for a producer block arriving after a nominal pop; see AmbiqAI/heartkit-vitals-demo#36. */
static_assert(TIO_ECG_TX_HIGH_WATER - TIO_ECG_SAMPLES_PER_PKT + TIO_ECG_TX_BLOCK_SAMPLES <= ECG_TX_BUF_LEN,
              "ECG TX peak occupancy (H - pkt + block) exceeds ring capacity");
static_assert(TIO_PPG_TX_HIGH_WATER - TIO_PPG_SAMPLES_PER_PKT + TIO_PPG_TX_BLOCK_SAMPLES <= PPG_TX_BUF_LEN,
              "PPG TX peak occupancy (H - pkt + block) exceeds ring capacity");

/* Include servo deadband, packet phase, and producer slip in the peak bound. */
static_assert(TIO_ECG_TX_TROUGH_TARGET + TIO_TX_SERVO_PULL_DIV + TIO_ECG_SAMPLES_PER_PKT + TIO_TX_SLIP_SAMPLES +
                      TIO_ECG_TX_BLOCK_SAMPLES <=
                  TIO_ECG_TX_HIGH_WATER,
              "ECG worst-case trough + slip + block must fit under H");
static_assert(TIO_PPG_TX_TROUGH_TARGET + TIO_TX_SERVO_PULL_DIV + TIO_PPG_SAMPLES_PER_PKT + TIO_TX_SLIP_SAMPLES +
                      TIO_PPG_TX_BLOCK_SAMPLES <=
                  TIO_PPG_TX_HIGH_WATER,
              "PPG worst-case trough + slip + block must fit under H");

/* The trough must leave a whole packet in hand, or the pump would skip on
 * every tick that sits at target -- which is the emission gap this exists to
 * prevent. */
static_assert(TIO_ECG_TX_TROUGH_TARGET > TIO_ECG_SAMPLES_PER_PKT, "ECG trough target must exceed one packet");
static_assert(TIO_PPG_TX_TROUGH_TARGET > TIO_PPG_SAMPLES_PER_PKT, "PPG trough target must exceed one packet");

/* Span multiple producer blocks so the trough estimate rejects block-phase aliasing. */
static_assert(TIO_TX_SERVO_WINDOW_TICKS >= (3 * TIO_ECG_TX_BLOCK_SAMPLES / TIO_ECG_SAMPLES_PER_PKT),
              "servo window must span at least three ECG producer blocks");

/* Payload must stay within what the TileIO slot framing accepts (240 B was the
 * bound the pre-fix senders were written against). */
static_assert(TIO_ECG_MAX_SAMPLES_PER_PKT * 3 * sizeof(int16_t) <= 240, "ECG max payload too large");
static_assert(TIO_PPG_MAX_SAMPLES_PER_PKT * 3 * sizeof(int16_t) <= 240, "PPG max payload too large");

/* Samples every ring in the group holds in common. Using the minimum (never a
 * per-ring length) is what keeps the group advancing as a unit. */
static size_t
tio_tx_group_avail(const tio_tx_group_t *group)
{
    size_t avail = ringbuffer_len(group->rings[0]);
    for (uint8_t i = 1; i < group->numRings; i++) {
        avail = MIN(avail, ringbuffer_len(group->rings[i]));
    }
    return avail;
}

/* Correct clock drift using trough change and position error, with a bounded
 * extra-sample budget; see AmbiqAI/heartkit-vitals-demo#12. */
static void
tio_tx_group_servo(tio_tx_group_t *group)
{
    int32_t trough = (int32_t)group->servoTroughMin;
    int32_t rate = trough - (int32_t)group->servoTroughPrev;
    int32_t pull = (trough - (int32_t)group->troughTarget) / TIO_TX_SERVO_PULL_DIV;
    int32_t budget = (int32_t)group->extraBudget + rate + pull;

    if (budget < 0) {
        budget = 0;
    } else if (budget > TIO_TX_SERVO_MAX_BUDGET) {
        budget = TIO_TX_SERVO_MAX_BUDGET;
    }
    group->extraBudget = (uint16_t)budget;

    group->servoTroughPrev = (uint32_t)trough;
    group->servoTrough = (uint32_t)trough;
    group->servoTroughMin = TIO_OCC_MIN_INIT;
    group->extraSpent = 0;
    group->servoTicks = 0;
}

/* Trim to high-water, then decide whether and how much to emit. Returns the
 * number of samples to pop from EVERY ring of the group, or 0 to emit nothing
 * this tick. Never returns less than samplesPerPkt (no short packets) and
 * never more than samplesPerPkt + TIO_TX_DRIFT_CATCHUP_SAMPLES. */
static size_t
tio_tx_group_prepare(tio_tx_group_t *group)
{
    size_t avail = tio_tx_group_avail(group);
    /* Sample occupancy before the trim and before the pop, so the watermarks
     * reflect what the producer actually created. */
    hkv_gauge_observe(group->gaugeOcc, (uint32_t)avail);
    /* Servo error signal: the minimum occupancy seen this window, sampled at
     * tick start so it reflects what the pump actually had to work with. */
    if ((uint32_t)avail < group->servoTroughMin) {
        group->servoTroughMin = (uint32_t)avail;
    }
    if (++group->servoTicks >= TIO_TX_SERVO_WINDOW_TICKS) {
        tio_tx_group_servo(group);
    }
    if (avail > group->highWater) {
        /* Discard the stale oldest samples, keep the newest H. One `drop`,
         * applied to every ring, so the group stays aligned across the trim. */
        size_t drop = avail - group->highWater;
        for (uint8_t i = 0; i < group->numRings; i++) {
            ringbuffer_seek(group->rings[i], drop);
        }
        hkv_count_n(group->cntTrimmed, (uint32_t)drop);
        avail = group->highWater;
    }
    if (avail < group->samplesPerPkt) {
        /* Producer running fractionally slow, or simply nothing new yet. Skip
         * the tick rather than emit a short packet. */
        return 0;
    }
    /* Keep a full-packet reserve above the trough before spending extra-sample budget. */
    size_t numSamples = group->samplesPerPkt;
    if (group->extraSpent < group->extraBudget &&
        avail > (size_t)(group->troughTarget + group->samplesPerPkt)) {
        numSamples = group->samplesPerPkt + TIO_TX_DRIFT_CATCHUP_SAMPLES;
        if (numSamples > avail) {
            numSamples = avail;
        }
        group->extraSpent++;
        hkv_count(group->cntDrained);
    }
    return numSamples;
}

/* Drop missed periods instead of emitting catch-up bursts; see AmbiqAI/heartkit-vitals-demo#12. */
static void
tio_pump_wait(TickType_t *pLastWake)
{
    const TickType_t period = pdMS_TO_TICKS(TIO_PUMP_INTERVAL_MS);
    TickType_t now = xTaskGetTickCount();
    /* A phase-offset anchor can be in the future; signed subtraction preserves that case. */
    if ((int32_t)(now - *pLastWake) >= (int32_t)period) {
        *pLastWake = now - period;
    }
    vTaskDelayUntil(pLastWake, period);
}

static void
send_ecg_signals(void)
{
    /* Sized for the drift-drain maximum, not the nominal packet. */
    uint8_t buffer[TIO_ECG_MAX_SAMPLES_PER_PKT * (sizeof(uint16_t) + 2 * sizeof(int16_t))];
    float32_t rawVal, denVal;
    uint16_t maskVal;
    int16_t txVal;
    uint32_t length;
    /* Single shared count for all three rings -- see the alignment note above. */
    size_t numSamples = tio_tx_group_prepare(&g_ecgTxGroup);
    if (numSamples == 0) {
        hkv_count(HKV_CNT_TIO_SLOT(0, HKV_TIO_WHICH_NODATA));
        return;
    }
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
    hkv_count(g_ecgTxGroup.cntPumped);
    if (pack_and_enqueue_tio_packet(0, 0, buffer, length)) {
        hkv_count(g_ecgTxGroup.cntDelivered);
    }
}

/* Display rates must not reuse bypass timings needed by battery duty accounting. */
static volatile float g_aiIps[3] = {NAN, NAN, NAN};

static void
send_ecg_metrics(void)
{
    float32_t buffer[16];
    buffer[0] = ecgMetResults.hr;
    buffer[1] = ecgMetResults.hrv;
    buffer[2] = ecgMetResults.denoiseCossim;
    buffer[3] = ecgMetResults.arrhythmiaLabel;
    buffer[4] = ai_display_rate(g_aiIps[0], appState.denoiseMode == DenoiseModeAi);
    buffer[5] = ai_display_rate(g_aiIps[1], appState.segMode == SegmentationModeAi);
    buffer[6] = ai_display_rate(g_aiIps[2], appState.arrMode == ArrhythmiaModeAi);
    buffer[7] = ecgMetResults.qos;
    const float powerMw = inference_power_mw(appState.speedMode != 0);
    buffer[8] = 1.0e3f * buffer[4] / powerMw;
    buffer[9] = 1.0e3f * buffer[5] / powerMw;
    buffer[10] = 1.0e3f * buffer[6] / powerMw;
    pack_and_enqueue_tio_packet(0, 1, buffer, 11 * sizeof(float32_t));
}

typedef struct {
    float32_t baseline;
    float32_t previous;
    bool initialized;
} ppg_tx_display_state_t;

static ppg_tx_display_state_t g_ppg_tx_display[2] = {0};

/* Remove DC drift from the display only. A large single-sample change is an
 * AGC LED-current step, so rebase immediately instead of drawing it as a
 * discontinuity in the browser waveform. */
static float32_t
ppg_display_sample(ppg_tx_display_state_t *state, float32_t sample)
{
    if (!state->initialized) {
        state->baseline = sample;
        state->previous = sample;
        state->initialized = true;
        return 0.0f;
    }

    float32_t delta = sample - state->previous;
    if (fabsf(delta) > PPG_TX_STEP_THRESHOLD) {
        state->baseline += delta;
    }
    state->baseline += PPG_TX_BASELINE_ALPHA * (sample - state->baseline);
    state->previous = sample;
    return (sample - state->baseline) * PPG_TX_GAIN;
}

static void
send_ppg_signals(void)
{
    /* Sized for the drift-drain maximum, not the nominal packet. */
    uint8_t buffer[TIO_PPG_MAX_SAMPLES_PER_PKT * (sizeof(uint16_t) + 2 * sizeof(int16_t))];
    float32_t val1, val2;
    float32_t txVal1, txVal2;
    int16_t txValI16;
    uint32_t length;
    uint8_t qos = (uint8_t)(ppgMetResults.qos / 25);
    uint16_t mask = (uint16_t)(qos << SIG_MASK_QOS_OFFSET);
    /* Single shared count for both rings -- see the alignment note above. */
    size_t numSamples = tio_tx_group_prepare(&g_ppgTxGroup);
    if (numSamples == 0) {
        hkv_count(HKV_CNT_TIO_SLOT(1, HKV_TIO_WHICH_NODATA));
        return;
    }
    length = 0;
    for (size_t i = 0; i < numSamples; i++) {
        memcpy(&buffer[length], &mask, sizeof(uint16_t));
        length += sizeof(uint16_t);
        ringbuffer_pop(&rbPpg1Tx, &val1, 1);
        txVal1 = CLIP(ppg_display_sample(&g_ppg_tx_display[0], val1), -32768.0f, 32767.0f);
        txValI16 = (int16_t)txVal1;
        memcpy(&buffer[length], &txValI16, sizeof(int16_t));
        length += sizeof(int16_t);
        ringbuffer_pop(&rbPpg2Tx, &val2, 1);
        txVal2 = CLIP(ppg_display_sample(&g_ppg_tx_display[1], val2), -32768.0f, 32767.0f);
        txValI16 = (int16_t)txVal2;
        memcpy(&buffer[length], &txValI16, sizeof(int16_t));
        length += sizeof(int16_t);
    }
    hkv_count(g_ppgTxGroup.cntPumped);
    if (pack_and_enqueue_tio_packet(1, 0, buffer, length)) {
        hkv_count(g_ppgTxGroup.cntDelivered);
    }
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
        hkv_count(HKV_CNT_TIO_SLOT(2, HKV_TIO_WHICH_NODATA));
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
    appMetResults.avgAiIps = ai_average_rate(
        ai_display_rate(g_aiIps[0], appState.denoiseMode == DenoiseModeAi),
        ai_display_rate(g_aiIps[1], appState.segMode == SegmentationModeAi),
        ai_display_rate(g_aiIps[2], appState.arrMode == ArrhythmiaModeAi));
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
        /* Bounded wait, not portMAX_DELAY: a stopped measurement stops the INT
         * line too, so the recovery check has to run off a timeout as well. */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(AS7058_SENSOR_TASK_POLL_MS)) > 0) {
            sensor_process_irq_events();
        }
        sensor_service_recovery();
    }
}

// ECG processing.

/* Count stage failures independently of optional diagnostic output. */

void
EcgProcessTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t err = 0;
    uint32_t tickStart;
    /* Bracket the model invoke alone. tickStart spans the whole stage (peeks,
     * DSP filter, metrics) and feeds *Ips; the *_lat_us fields must not carry
     * that. Zero in a DSP-mode stage means "no model ran", not "no time". #37 */
    uint32_t modelStart;
    uint32_t modelLatUs;
    size_t numSamples;
    TickType_t pumpLastWake = xTaskGetTickCount();

    while (true) {
        err = 0;
        service_ecg_flush_request();

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
            modelLatUs = 0;
            if (appState.denoiseMode == DenoiseModeAi) {
                modelStart = dwt_cycles();
                err = ecg_denoise_inference(ecgDenInout, ecgDenInout, 0, ECG_DEN_THRESHOLD);
                modelLatUs = dwt_delta_us(modelStart);
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

            ecgMetResults.denoiseIps = ips_from_delta_us(dwt_delta_us(tickStart));
            g_aiIps[0] = ai_display_rate(ecgMetResults.denoiseIps, modelLatUs > 0 && err == 0);
            ecgMetResults.denoiseLatUs = modelLatUs;
            ecgMetResults.denoiseLatMaxUs = MAX(ecgMetResults.denoiseLatMaxUs, modelLatUs);
            /* Publish duration before the run counter so duty sampling cannot pair a new
             * run with an old duration. The compiler barrier enforces store ordering. */
            __asm volatile("" ::: "memory");
            hkv_count(HKV_CNT_PIPE_DEN_RUNS);

            ecgMetResults.denoiseuIpspw =
                1.0e3f * ecgMetResults.denoiseIps / inference_power_mw(appState.speedMode != 0);
            if (err != 0) {
                hkv_count(HKV_CNT_PIPE_ERR_ECG_DEN);
            }
            HKV_TRACE_KV("ecg", "den_err", err);
        }

        ///////////////////////////////////////////////////////////////////
        // ECG SEGMENTATION
        ///////////////////////////////////////////////////////////////////
        else if (ringbuffer_len(&rbEcgSeg) >= ECG_SEG_WINDOW_LEN) {
            tickStart = dwt_cycles();
            ringbuffer_peek(&rbEcgSeg, ecgSegInout, ECG_SEG_WINDOW_LEN);

            modelLatUs = 0;
            if (appState.segMode == SegmentationModeDsp) {
                /* Preserve quality annotations from the shared DSP segmentation path. */
                err = ecg_physiokit_segmentation_inference(ecgSegInout, ecgSegMask, 0, &ecgMetResults.qos);
            } else if (appState.segMode == SegmentationModeAi) {
                modelStart = dwt_cycles();
                err = ecg_segmentation_inference(ecgSegInout, ecgSegMask, 0, ECG_SEG_THRESHOLD, &ecgMetResults.qos);
                modelLatUs = dwt_delta_us(modelStart);
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

            ecgMetResults.segmentIps = ips_from_delta_us(dwt_delta_us(tickStart));
            g_aiIps[1] = ai_display_rate(ecgMetResults.segmentIps, modelLatUs > 0 && err == 0);
            ecgMetResults.segmentLatUs = modelLatUs;
            ecgMetResults.segmentLatMaxUs = MAX(ecgMetResults.segmentLatMaxUs, modelLatUs);
            /* Counter after duration, barrier required -- see the denoise
             * branch for why source order alone does not bind the compiler. */
            __asm volatile("" ::: "memory");
            hkv_count(HKV_CNT_PIPE_SEG_RUNS);
            /* Follows the operating point -- see the denoise branch. */
            ecgMetResults.segmentuIpspw =
                1.0e3f * ecgMetResults.segmentIps / inference_power_mw(appState.speedMode != 0);
            if (err != 0) {
                hkv_count(HKV_CNT_PIPE_ERR_ECG_SEG);
            }
            HKV_TRACE_KV("ecg", "seg_err", err);
        }

        ///////////////////////////////////////////////////////////////////
        // ECG ARRHYTHMIA + METRICS
        ///////////////////////////////////////////////////////////////////
        else if (MIN(ringbuffer_len(&rbEcgMet), ringbuffer_len(&rbEcgMaskMet)) >= ECG_MET_WINDOW_LEN) {
            uint32_t arrErr = 0;
            tickStart = dwt_cycles();
            ringbuffer_peek(&rbEcgMet, ecgMetData, ECG_MET_WINDOW_LEN);
            ringbuffer_peek(&rbEcgMaskMet, ecgMaskMetData, ECG_MET_WINDOW_LEN);

            err = metrics_capture_ecg(&metricsCfg, ecgMetData, ecgMaskMetData, ECG_MET_WINDOW_LEN, &ecgMetResults);

            modelLatUs = 0;
            if (appState.arrMode == ArrhythmiaModeDsp) {
                ecgMetResults.arrhythmiaLabel =
                    ecgMetResults.hr < 40 ? ECG_ARR_SB : ecgMetResults.hr > 100 ? ECG_ARR_GSVT : ECG_ARR_SR;
            } else if (appState.arrMode == ArrhythmiaModeAi) {
                uint32_t arrLabel = ECG_ARR_INCONCLUSIVE;
                modelStart = dwt_cycles();
                arrErr = ecg_arrhythmia_inference(ecgMetData, ECG_ARR_THRESHOLD, &arrLabel);
                modelLatUs = dwt_delta_us(modelStart);
                ecgMetResults.arrhythmiaLabel = (float32_t)arrLabel;
            } else {
                ecgMetResults.arrhythmiaLabel = 0;
            }

            ringbuffer_seek(&rbEcgMet, ECG_MET_VALID_LEN);
            ringbuffer_seek(&rbEcgMaskMet, ECG_MET_VALID_LEN);

            ecgMetResults.arrhythmiaIps = ips_from_delta_us(dwt_delta_us(tickStart));
            g_aiIps[2] = ai_display_rate(ecgMetResults.arrhythmiaIps, modelLatUs > 0 && arrErr == 0);
            ecgMetResults.arrhythmiaLatUs = modelLatUs;
            ecgMetResults.arrhythmiaLatMaxUs = MAX(ecgMetResults.arrhythmiaLatMaxUs, modelLatUs);
            /* Counter after duration, barrier required -- see the denoise
             * branch. This is the site where GCC was observed sinking the
             * store past the volatile increment. */
            __asm volatile("" ::: "memory");
            hkv_count(HKV_CNT_PIPE_MET_RUNS);
            /* Follows the operating point -- see the denoise branch. */
            ecgMetResults.arrhythmiaIpspw =
                1.0e3f * ecgMetResults.arrhythmiaIps / inference_power_mw(appState.speedMode != 0);

            send_ecg_metrics();
            if (err != 0) {
                hkv_count(HKV_CNT_PIPE_ERR_ECG_MET);
            }
            if (arrErr != 0) {
                hkv_count(HKV_CNT_PIPE_ERR_ECG_ARR);
                HKV_TRACE_KV("ecg", "arr_err", arrErr);
            }
            HKV_TRACE_KV("ecg", "met_err", err);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        (void)err;

        /* Delay before enqueueing to avoid back-to-back packets after an overrun. */
        tio_pump_wait(&pumpLastWake);
        send_ecg_signals();
    }
}

// PPG processing.

/* Track producer activity separately from samples accepted by the TX taps. */

void
PpgProcessTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t err = 0;
    bio_spo2_a0_configuration_t spo2Cfg;
    const bio_spo2_a0_configuration_t *pSpo2Cfg;
    /* Phase-stagger the PPG pump against the ECG pump. Both tasks are created
     * back-to-back at equal priority and would otherwise anchor to the same
     * tick and enqueue into the transport in the same millisecond every time,
     * concentrating the packet budget into bursts instead of spreading it. */
    TickType_t pumpLastWake = xTaskGetTickCount() + pdMS_TO_TICKS(TIO_PPG_PUMP_PHASE_MS);

    while (true) {
        hkv_count(HKV_CNT_PIPE_PPG_ITERS);
        service_ppg_flush_request();
        size_t numSamples = MIN(ringbuffer_len(&rbPpg1Sensor), ringbuffer_len(&rbPpg2Sensor));
        hkv_gauge_observe(HKV_GAUGE_PPG_TEE, (uint32_t)(numSamples / PPG_DS_RATE));
        for (size_t i = 0; i < numSamples / PPG_DS_RATE; i++) {
            float32_t sample1, sample2;
            ringbuffer_seek(&rbPpg1Sensor, PPG_DS_RATE - 1);
            ringbuffer_peek(&rbPpg1Sensor, &sample1, 1);
            ringbuffer_push(&rbPpg1Met, &sample1, 1);
            ringbuffer_push(&rbPpg1Tx, &sample1, 1);
            ringbuffer_seek(&rbPpg1Sensor, 1);

            ringbuffer_seek(&rbPpg2Sensor, PPG_DS_RATE - 1);
            ringbuffer_peek(&rbPpg2Sensor, &sample2, 1);
            ringbuffer_push(&rbPpg2Met, &sample2, 1);
            ringbuffer_push(&rbPpg2Tx, &sample2, 1);
            ringbuffer_seek(&rbPpg2Sensor, 1);
            hkv_count(HKV_CNT_PIPE_PPG_PUSHED);
        }

        if (MIN(ringbuffer_len(&rbPpg1Met), ringbuffer_len(&rbPpg2Met)) >= PPG_MET_WINDOW_LEN) {
            ringbuffer_peek(&rbPpg1Met, ppg1MetData, PPG_MET_WINDOW_LEN);
            ringbuffer_peek(&rbPpg2Met, ppg2MetData, PPG_MET_WINDOW_LEN);

            pSpo2Cfg = sensor_get_spo2_config(&spo2Cfg) ? &spo2Cfg : NULL;
            err = metrics_capture_ppg(&metricsCfg, ppg1MetData, ppg2MetData, PPG_MET_WINDOW_LEN, pSpo2Cfg, &ppgMetResults);

            ringbuffer_seek(&rbPpg1Met, PPG_MET_VALID_LEN);
            ringbuffer_seek(&rbPpg2Met, PPG_MET_VALID_LEN);
            send_ppg_metrics();
            if (err != 0) {
                hkv_count(HKV_CNT_PIPE_ERR_PPG_MET);
            }
            HKV_TRACE_KV("ppg", "met_err", err);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        (void)err; /* counted above; only printed under EN_APP_TRACE */

        /* Wait before sending -- see the note in EcgProcessTask. */
        tio_pump_wait(&pumpLastWake);
        send_ppg_signals();
    }
}

// CPU utilization.

///////////////////////////////////////////////////////////////////////////////
// Battery projection
///////////////////////////////////////////////////////////////////////////////
// Busy time comes from runtime statistics; stage weights come from timed run
// counts. Their rolling windows must stay aligned. Async IOM activity can
// overlap idle time, so quiet sleep remains a deployment assumption, not a
// measurement of the streaming application. See AmbiqAI/heartkit-vitals-demo#68.

/* DSP/off paths still count as stage work and use the stage power profile.
 * See AmbiqAI/heartkit-vitals-demo#68. */

/* One attribution term's 30 s history, advanced from the SAME ring index and
 * fill state as the utilisation ring so every term describes one window. See #8. */
typedef struct {
    float32_t samples[kCpuStatsRollingSeconds];
    float32_t sum;
} cpu_rolling_t;

static inline void
cpu_rolling_put(cpu_rolling_t *ring, uint32_t index, bool filled, float32_t value)
{
    if (filled) {
        ring->sum -= ring->samples[index];
    }
    ring->samples[index] = value;
    ring->sum += value;
}

void
CpuProcessTask(void *pvParameters)
{
    (void)pvParameters;
    const uint32_t samplesPerPublish = kCpuStatsPublishPeriodMs / kCpuStatsSamplePeriodMs;
    /* Include loop work in elapsed time so duration-based duty is not overestimated. */
    TickType_t publishWindowStart = xTaskGetTickCount();
    float32_t cpuUtilSecondAccum = 0.0f;
    uint32_t cpuUtilSecondCount = 0;
    float32_t cpuUtilRolling[kCpuStatsRollingSeconds] = {0};
    float32_t cpuUtilRollingSum = 0.0f;
    uint32_t cpuUtilRollingCount = 0;
    uint32_t cpuUtilRollingIndex = 0;
    /* Inference-duty ring, advanced in lockstep with cpuUtilRolling above so
     * both terms of the model describe the same 30 s window. */
    float32_t infFracRolling[kCpuStatsRollingSeconds] = {0};
    float32_t infFracRollingSum = 0.0f;
    cpu_rolling_t denDutyRolling = {};
    cpu_rolling_t segDutyRolling = {};
    cpu_rolling_t arrDutyRolling = {};
    uint32_t prevDenRuns = g_hkv_counters[HKV_CNT_PIPE_DEN_RUNS];
    uint32_t prevSegRuns = g_hkv_counters[HKV_CNT_PIPE_SEG_RUNS];
    uint32_t prevMetRuns = g_hkv_counters[HKV_CNT_PIPE_MET_RUNS];
    uint32_t runTimeTicks = 0;
    float32_t ecgTaskPerc = 0, ppgTaskPerc = 0, totalTaskPerc = 0;
    /* Attribution terms (issue #8): sensor capture and TileIO transmit are whole
     * tasks, so they come from the same run-time counters as the ECG/PPG pair. */
    cpu_rolling_t capRolling = {};
    cpu_rolling_t txRolling = {};
    cpu_rolling_t projInfRolling = {};
    float32_t capSecondAccum = 0.0f;
    float32_t txSecondAccum = 0.0f;
    uint32_t prevRun = 0, prevEcg = 0, prevPpg = 0, prevIdle = 0, prevCap = 0, prevTx = 0;
    uint32_t runDelta, ecgDelta, ppgDelta, idleDelta;
    float32_t capTaskPerc = 0, txTaskPerc = 0;
    size_t numTasks;

    while (true) {
        uint32_t idleCounter = 0;
        service_cpu_flush_request();
        numTasks = uxTaskGetSystemState(xTaskDetails, HKV_TASK_STATUS_CAPACITY, &runTimeTicks);
        if (numTasks == 0) {
            /* An undersized snapshot must not publish an empty task list as CPU utilization. */
            hkv_count(HKV_CNT_CPU_STAT_OVERFLOW);
            /* Discarding the interval means discarding the stage runs inside
             * it too, exactly as the prevRun re-baseline below does. Without
             * this, the runs that happened during a skipped interval land in
             * the next published window and inflate its inference duty. */
            prevDenRuns = g_hkv_counters[HKV_CNT_PIPE_DEN_RUNS];
            prevSegRuns = g_hkv_counters[HKV_CNT_PIPE_SEG_RUNS];
            prevMetRuns = g_hkv_counters[HKV_CNT_PIPE_MET_RUNS];
            publishWindowStart = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(kCpuStatsSamplePeriodMs));
            continue;
        }
        g_cpu_num_tasks = (uint32_t)numTasks;
        runDelta = runTimeTicks - prevRun;
        if (prevRun == 0 || runDelta == 0) {
            prevRun = runTimeTicks;
            prevIdle = 0;
            for (size_t i = 0; i < numTasks; i++) {
                if (xTaskDetails[i].xHandle == ecgProcessTaskHandle) {
                    prevEcg = xTaskDetails[i].ulRunTimeCounter;
                } else if (xTaskDetails[i].xHandle == ppgProcessTaskHandle) {
                    prevPpg = xTaskDetails[i].ulRunTimeCounter;
                } else if (xTaskDetails[i].xHandle == sensorIrqTaskHandle) {
                    prevCap = xTaskDetails[i].ulRunTimeCounter;
                } else if (xTaskDetails[i].xHandle == tioProcessTaskHandle) {
                    prevTx = xTaskDetails[i].ulRunTimeCounter;
                }
                if (xTaskDetails[i].uxCurrentPriority == tskIDLE_PRIORITY) {
                    prevIdle += xTaskDetails[i].ulRunTimeCounter;
                }
            }
            /* Same reason the run-time counters are re-baselined here: this
             * interval is discarded, so any stage runs that happened during
             * startup must not land in the first published window. */
            prevDenRuns = g_hkv_counters[HKV_CNT_PIPE_DEN_RUNS];
            prevSegRuns = g_hkv_counters[HKV_CNT_PIPE_SEG_RUNS];
            prevMetRuns = g_hkv_counters[HKV_CNT_PIPE_MET_RUNS];
            publishWindowStart = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        prevRun = runTimeTicks;

        ecgDelta = 0;
        ppgDelta = 0;
        ecgTaskPerc = 0;
        ppgTaskPerc = 0;
        capTaskPerc = 0;
        txTaskPerc = 0;
        for (size_t i = 0; i < numTasks; i++) {
            if (xTaskDetails[i].xHandle == ecgProcessTaskHandle) {
                ecgDelta = xTaskDetails[i].ulRunTimeCounter - prevEcg;
                prevEcg = xTaskDetails[i].ulRunTimeCounter;
                ecgTaskPerc = 100.0f * (float32_t)ecgDelta / (float32_t)runDelta;
            } else if (xTaskDetails[i].xHandle == ppgProcessTaskHandle) {
                ppgDelta = xTaskDetails[i].ulRunTimeCounter - prevPpg;
                prevPpg = xTaskDetails[i].ulRunTimeCounter;
                ppgTaskPerc = 100.0f * (float32_t)ppgDelta / (float32_t)runDelta;
            } else if (xTaskDetails[i].xHandle == sensorIrqTaskHandle) {
                capTaskPerc = 100.0f * (float32_t)(xTaskDetails[i].ulRunTimeCounter - prevCap) / (float32_t)runDelta;
                prevCap = xTaskDetails[i].ulRunTimeCounter;
            } else if (xTaskDetails[i].xHandle == tioProcessTaskHandle) {
                txTaskPerc = 100.0f * (float32_t)(xTaskDetails[i].ulRunTimeCounter - prevTx) / (float32_t)runDelta;
                prevTx = xTaskDetails[i].ulRunTimeCounter;
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
        capSecondAccum += capTaskPerc;
        txSecondAccum += txTaskPerc;
        cpuUtilSecondCount++;
        totalTaskPerc = ecgTaskPerc + ppgTaskPerc;

        if (cpuUtilSecondCount >= samplesPerPublish) {
            float32_t cpuUtilSecondAvg = cpuUtilSecondAccum / (float32_t)cpuUtilSecondCount;
            float32_t capSecondAvg = capSecondAccum / (float32_t)cpuUtilSecondCount;
            float32_t txSecondAvg = txSecondAccum / (float32_t)cpuUtilSecondCount;
            cpuUtilSecondAccum = 0.0f;
            capSecondAccum = 0.0f;
            txSecondAccum = 0.0f;
            cpuUtilSecondCount = 0;

            /* Inference duty for this window: each stage's own measured run
             * rate x its own measured duration. Counters are free-running and
             * never reset by the reporter, so a local delta is the window. */
            /* DWT duration and reporting ticks must share the synchronized
             * core timebase across speed changes. See AmbiqAI/heartkit-vitals-demo#25. */
            const TickType_t windowEndTicks = xTaskGetTickCount();
            const float32_t publishWindowSec =
                (float32_t)(uint32_t)(windowEndTicks - publishWindowStart) / (float32_t)configTICK_RATE_HZ;
            publishWindowStart = windowEndTicks;
            const uint32_t denRuns = g_hkv_counters[HKV_CNT_PIPE_DEN_RUNS];
            const uint32_t segRuns = g_hkv_counters[HKV_CNT_PIPE_SEG_RUNS];
            const uint32_t metRuns = g_hkv_counters[HKV_CNT_PIPE_MET_RUNS];
            /* Kept per stage rather than summed on the spot: the projection
             * scales each one by its own duty factor. See #8. */
            const float32_t denFrac = stage_duty_frac(denRuns - prevDenRuns, ecgMetResults.denoiseIps, publishWindowSec);
            const float32_t segFrac = stage_duty_frac(segRuns - prevSegRuns, ecgMetResults.segmentIps, publishWindowSec);
            const float32_t metFrac =
                stage_duty_frac(metRuns - prevMetRuns, ecgMetResults.arrhythmiaIps, publishWindowSec);
            const float32_t infFracInstant = denFrac + segFrac + metFrac;
            const float32_t projInfPctInstant =
                hkv_duty_inference_pct(100.0f * denFrac, 100.0f * segFrac, 100.0f * metFrac);
            prevDenRuns = denRuns;
            prevSegRuns = segRuns;
            prevMetRuns = metRuns;

            const bool rollingFilled = (cpuUtilRollingCount >= kCpuStatsRollingSeconds);
            cpu_rolling_put(&capRolling, cpuUtilRollingIndex, rollingFilled, capSecondAvg);
            cpu_rolling_put(&txRolling, cpuUtilRollingIndex, rollingFilled, txSecondAvg);
            cpu_rolling_put(&projInfRolling, cpuUtilRollingIndex, rollingFilled, projInfPctInstant);
            cpu_rolling_put(&denDutyRolling, cpuUtilRollingIndex, rollingFilled, denFrac);
            cpu_rolling_put(&segDutyRolling, cpuUtilRollingIndex, rollingFilled, segFrac);
            cpu_rolling_put(&arrDutyRolling, cpuUtilRollingIndex, rollingFilled, metFrac);
            if (cpuUtilRollingCount < kCpuStatsRollingSeconds) {
                cpuUtilRolling[cpuUtilRollingIndex] = cpuUtilSecondAvg;
                cpuUtilRollingSum += cpuUtilSecondAvg;
                infFracRolling[cpuUtilRollingIndex] = infFracInstant;
                infFracRollingSum += infFracInstant;
                cpuUtilRollingCount++;
            } else {
                cpuUtilRollingSum -= cpuUtilRolling[cpuUtilRollingIndex];
                cpuUtilRolling[cpuUtilRollingIndex] = cpuUtilSecondAvg;
                cpuUtilRollingSum += cpuUtilSecondAvg;
                infFracRollingSum -= infFracRolling[cpuUtilRollingIndex];
                infFracRolling[cpuUtilRollingIndex] = infFracInstant;
                infFracRollingSum += infFracInstant;
            }
            cpuUtilRollingIndex = (cpuUtilRollingIndex + 1) % kCpuStatsRollingSeconds;

            appMetResults.cpuPercUtil = cpuUtilRollingSum / (float32_t)cpuUtilRollingCount;

            /* Measured and projected reported separately, never blended
             * (issue #8). The split is coarse by design: PPG stage time and DSP
             * paths have no per-stage counters, so they land in `other`. */
            const float32_t windowCount = (float32_t)cpuUtilRollingCount;
            const float32_t capturePerc = capRolling.sum / windowCount;
            const float32_t transportPerc = txRolling.sum / windowCount;
            appMetResults.cpuSplit = hkv_cpu_split(appMetResults.cpuPercUtil, capturePerc,
                                                   100.0f * infFracRollingSum / windowCount, transportPerc);
            appMetResults.cpuProjPerc = hkv_cpu_proj_pct(capturePerc, projInfRolling.sum / windowCount);

            const hkv_battery_estimate_t battery = hkv_battery_estimate(
                hkv_battery_profile(appState.speedMode != 0), appMetResults.cpuPercUtil / 100.0f,
                denDutyRolling.sum / windowCount, segDutyRolling.sum / windowCount,
                arrDutyRolling.sum / windowCount);
            appMetResults.battInferenceFrac = battery.inference_fraction;
            appMetResults.battAvgPowerMw = battery.average_mw;
            appMetResults.batteryDays = battery.days;

            send_cpu_metrics();
        }

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

/* Preserve a pending packet across retries; see AmbiqAI/heartkit-vitals-demo#56. */
static_assert(TIO_TX_SM_PACKET_LEN == TIO_USB_PACKET_LEN, "tio_tx_sm.h packet size must match the TileIO frame");

/* BLE fan-out state, refreshed per iteration and read by the dequeue hook. */
typedef struct
{
    bool bleReady;
} tio_tx_fanout_t;

static bool
tio_tx_queue_receive(void *user, uint8_t *packet)
{
    (void)user;
    return xQueueReceive(g_tioTxQueue, packet, kTioTxTaskPollTicks) == pdTRUE;
}

static tio_tx_send_result_t
tio_tx_send(void *user, uint8_t *packet)
{
    uint32_t status;
    (void)user;

    status = tio_usb_send_slot_packet(packet, TIO_USB_PACKET_LEN);
    if (status == NSX_STATUS_SUCCESS) {
        return TIO_TX_SEND_OK;
    }
    if ((status == NSX_USB_STATUS_BUSY) || (status == NSX_USB_STATUS_PARTIAL)) {
        return TIO_TX_SEND_BUSY;
    }
    return TIO_TX_SEND_FAIL;
}

static uint32_t
tio_tx_now_ms(void *user)
{
    (void)user;
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

static uint32_t
tio_tx_queue_depth(void *user)
{
    (void)user;
    return (uint32_t)uxQueueMessagesWaiting(g_tioTxQueue);
}

/* Second transport gets every packet the queue yields, whatever USB does. */
static void
tio_tx_on_dequeued(void *user, const uint8_t *packet)
{
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
    tio_tx_fanout_t *fanout = (tio_tx_fanout_t *)user;
    if (fanout->bleReady) {
        /* Non-blocking (see ble_bringup_send_slot_packet's doc comment):
         * never allowed to stall USB delivery. */
        ble_bringup_send_slot_packet((uint8_t *)packet, TIO_USB_PACKET_LEN);
    }
#else
    (void)user;
    (void)packet;
#endif
}

static void
tio_tx_on_retry(void *user, const uint8_t *packet)
{
    (void)user;
    count_tio_usb_event(HKV_USB_WHICH_RETRY, packet);
}

static void
tio_tx_on_drop(void *user, const uint8_t *packet)
{
    (void)user;
    count_tio_usb_event(HKV_USB_WHICH_DROP, packet);
}

static void
tio_tx_on_stall(void *user)
{
    (void)user;
    hkv_count(HKV_CNT_USB_STALL);
}

void
TioProcessTask(void *pvParameters)
{
    (void)pvParameters;
    tio_tx_sm_t usb;
    tio_tx_fanout_t fanout = {};
    const tio_tx_ops_t ops = {
        .queue_receive = &tio_tx_queue_receive,
        .send = &tio_tx_send,
        .now_ms = &tio_tx_now_ms,
        .queue_depth = &tio_tx_queue_depth,
        .on_dequeued = &tio_tx_on_dequeued,
        .on_retry = &tio_tx_on_retry,
        .on_drop = &tio_tx_on_drop,
        .on_stall = &tio_tx_on_stall,
        .stall_ms = kTioUsbStallThresholdMs,
        .hold_watermark = TIO_TX_USB_HOLD_WATERMARK,
        .user = &fanout,
    };

    tio_tx_sm_reset(&usb);
    while (true) {
        check_tio_state();
        if (g_uio_state_request_pending) {
            g_uio_state_request_pending = 0;
            send_uio_state();
        }
        /* Host UIO writes are only latched (flag+copy) in the ISR-context
         * callback; apply them here in task context. */
        apply_pending_uio_state();
        if (g_tioTxQueue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        bool usbReady = g_tio_available && tio_usb_tx_available();
        /* BLE is a second, independent consumer of the SAME queue item (not
         * a second queue): dual queues would double memory and complexity
         * for no benefit here, since both transports need every packet.
         * Draining once and fanning out to both transports (tio_tx_on_dequeued)
         * keeps a single-consumer queue with clean semantics, at the cost of
         * a disconnected BLE not being distinguishable from "no work yet" --
         * acceptable since bleReady already covers that case explicitly. */
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
        fanout.bleReady = ble_bringup_connected();
#else
        fanout.bleReady = false;
#endif
        if (!usbReady) {
            /* Nothing held can ever land now; start clean on the next connect. */
            tio_tx_sm_host_lost(&usb, &ops);
        }
        if (!usbReady && !fanout.bleReady) {
            /* Neither transport has anyone listening: leave packets queued
             * (bounded depth, producer-side drops apply) rather than draining
             * into the void -- matches the pre-BLE USB-only behavior exactly
             * when BLE is compiled out/disconnected. */
            vTaskDelay(kTioTxTaskPollTicks);
            continue;
        }
        tio_tx_sm_step(&usb, &ops, usbReady);
        if (usb.pending) {
            /* Retry pacing, and the release rate above the watermark: below it
             * the queue receive is skipped, so this loop has nothing else to
             * block on. */
            vTaskDelay(kTioTxTaskPollTicks);
        }
    }
}

// Rotate diagnostic output to avoid blocking the signal pumps in a single burst.

/* Every hkv_log_* call in these callbacks runs with the log lock already held
 * (hkv_report_subsystem took it). They must not call hkv_log_begin or
 * hkv_log_end: the mutex is not recursive and a nested begin deadlocks. */

static void
report_extra_sensor(void)
{
    /* Sensor accessors preserve ISR-owned interval accounting. */
    hkv_log_u32("isr", sensor_get_as7058_int_isr_count());
    hkv_log_u32("missed", sensor_get_irq_notify_missed_count());
    hkv_log_u32("ppg_push", sensor_get_ppg_push_count());
    hkv_log_u32("ppg_drop", sensor_get_ppg_drop_count());
    hkv_log_u32("ecg_push", sensor_get_ecg_push_count());
    hkv_log_u32("ecg_drop", sensor_get_ecg_drop_count());
    hkv_log_u32("isr_int_lo_ms", sensor_get_as7058_isr_min_interval_ms());
    hkv_log_u32("isr_int_hi_ms", sensor_get_as7058_isr_max_interval_ms());
    /* A queued read that errors or times out is reported here, not just to the
     * chiplib: the chiplib's own response is to stop the measurement, so
     * without this the fault would only show up as a silenced sensor.
     * `bus_sync` counts the reads that took the blocking fallback, which after
     * boot should stay flat. See #65. */
    hkv_log_u32("bus_err", sensor_bus_get_error_count());
    hkv_log_u32("bus_sync", sensor_bus_get_fallback_count());
    /* `bus_reset` counts IOM rebuilds after a read timed out with its transfer
     * still outstanding; each one costs the reads taken until the IOM went
     * quiet. See #67. */
    hkv_log_u32("bus_reset", sensor_bus_get_reset_count());
    /* `sens_restart` counts measurements restarted after the chiplib stopped
     * one on a read error. It should track bus_err; a bus_err that leaves it
     * flat means the stream is dead, not just gapped. See #67. */
    hkv_log_u32("sens_restart", sensor_get_restart_count());
    sensor_reset_as7058_isr_interval_stats();
}

static void
report_extra_tio(void)
{
    hkv_log_u32("qdepth", (uint32_t)uxQueueMessagesWaiting(g_tioTxQueue));
}

/* Budget is extra samples per servo window, not per second. A zero trough
 * does not distinguish slow production from a stopped producer. */
static void
report_extra_tx_group(const tio_tx_group_t *group)
{
    hkv_log_u32("bgt", group->extraBudget);
    hkv_log_u32("trgh", group->servoTrough);
    hkv_log_u32("hw", group->highWater);
}

static void
report_extra_txecg(void)
{
    report_extra_tx_group(&g_ecgTxGroup);
    /* Instantaneous TX-tap occupancy at the moment of sampling. Chronically 0
     * means the segmentation branch is not feeding the taps; large or climbing
     * means the taps fill but do not drain. */
    hkv_log_u32("raw_len", (uint32_t)ringbuffer_len(&rbEcgRawTx));
    hkv_log_u32("den_len", (uint32_t)ringbuffer_len(&rbEcgDenTx));
    hkv_log_u32("mask_len", (uint32_t)ringbuffer_len(&rbEcgMaskTx));
}

static void
report_extra_txppg(void)
{
    report_extra_tx_group(&g_ppgTxGroup);
    hkv_log_u32("p1_len", (uint32_t)ringbuffer_len(&rbPpg1Tx));
    hkv_log_u32("p2_len", (uint32_t)ringbuffer_len(&rbPpg2Tx));
}

static void
report_extra_ring(void)
{
    /* Stage-to-stage ring occupancy across both pipelines. Read as a profile
     * rather than individually: a single ring pinned near its capacity while
     * the ones after it sit empty localises the stalled stage immediately. */
    hkv_log_u32("ecg_sensor", (uint32_t)ringbuffer_len(&rbEcgSensor));
    hkv_log_u32("ecg_den", (uint32_t)ringbuffer_len(&rbEcgDen));
    hkv_log_u32("ecg_rawseg", (uint32_t)ringbuffer_len(&rbEcgRawSeg));
    hkv_log_u32("ecg_seg", (uint32_t)ringbuffer_len(&rbEcgSeg));
    hkv_log_u32("ecg_met", (uint32_t)ringbuffer_len(&rbEcgMet));
    hkv_log_u32("ecg_maskmet", (uint32_t)ringbuffer_len(&rbEcgMaskMet));
    hkv_log_u32("ppg1_sensor", (uint32_t)ringbuffer_len(&rbPpg1Sensor));
    hkv_log_u32("ppg2_sensor", (uint32_t)ringbuffer_len(&rbPpg2Sensor));
    hkv_log_u32("ppg1_met", (uint32_t)ringbuffer_len(&rbPpg1Met));
    hkv_log_u32("ppg2_met", (uint32_t)ringbuffer_len(&rbPpg2Met));
}

static void
report_extra_ecgmet(void)
{
    /* Fixed point, not the `%d.%02d` split this replaces: that idiom prints
     * -0.5 as "0.50" and costs an fabsf plus two float->int conversions per
     * field. See obs_fmt.h. */
    hkv_log_fx2("hr", ecgMetResults.hr);
    hkv_log_fx2("hrv", ecgMetResults.hrv);
    hkv_log_fx2("qos", ecgMetResults.qos);
    hkv_log_fx2("cossim", ecgMetResults.denoiseCossim);
    hkv_log_u32("rhythm", (uint32_t)ecgMetResults.arrhythmiaLabel);
}

static void
report_extra_ppgmet(void)
{
    hkv_log_fx2("pr", ppgMetResults.pr);
    hkv_log_fx2("spo2", ppgMetResults.spo2);
    hkv_log_fx2("qos", ppgMetResults.qos);
}

#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
/* Cache the stack scan outside the log mutex; only ReportTask accesses this value. */
static uint32_t g_cpu_ble_hwm = 0;
#endif

static void
report_extra_cpu(void)
{
    /* `tasks` against HKV_TASK_STATUS_CAPACITY is the headroom that the
     * stat_overflow counter on this same line reports the loss of. Watch it
     * before it becomes a number rather than after. */
    hkv_log_u32("tasks", g_cpu_num_tasks);
    hkv_log_u32("capacity", HKV_TASK_STATUS_CAPACITY);
    hkv_log_fx2("util", appMetResults.cpuPercUtil);
    hkv_log_fx2("batt_days", appMetResults.batteryDays);
    /* Battery-model breakdown (issue #17), so the three-state split is legible
     * on SWO instead of only its result. Only the two INDEPENDENT terms are
     * emitted: the other two are exact derivations of these and `util`, and
     * every extra fixed-point field lengthens the hold on the global log mutex.
     *   batt_cmp  = util - batt_inf   (compute % of wall time)
     *   batt_idle = 100 - util        (idle % of wall time)
     * `batt_inf` equal to `util` means the clamp is active, i.e. derived
     * inference duty exceeded measured busy. batt_inf is a percentage of wall
     * time; batt_pwr is the modelled average in mW at the LIVE speed mode
     * (see `speed_mode` on the `app` line -- the two must be read together). */
    hkv_log_fx2("batt_inf", 100.0f * appMetResults.battInferenceFrac);
    hkv_log_fx2("batt_pwr", appMetResults.battAvgPowerMw);
    hkv_log_fx2("avg_ips", appMetResults.avgAiIps);
    /* Model invoke duration, last run and maximum within THIS report interval,
     * in us. Measured, not derived from the *Ips rates. Zero means no invoke
     * COMPLETED in the interval: the stage is in DSP or off mode, or it is in
     * AI mode but its cadence is slower than the report rotation, which is the
     * common case. Read HKV_CNT_PIPE_*_RUNS to tell the two apart. See #37. */
    hkv_log_u32("den_lat_us", ecgMetResults.denoiseLatUs);
    hkv_log_u32("seg_lat_us", ecgMetResults.segmentLatUs);
    hkv_log_u32("arr_lat_us", ecgMetResults.arrhythmiaLatUs);
    hkv_log_u32("den_lat_max_us", ecgMetResults.denoiseLatMaxUs);
    hkv_log_u32("seg_lat_max_us", ecgMetResults.segmentLatMaxUs);
    hkv_log_u32("arr_lat_max_us", ecgMetResults.arrhythmiaLatMaxUs);
    /* Interval-scoped, so the window restarts here. A since-boot max would
     * latch a sample taken under the other operating point and keep reporting
     * it after a perf-mode switch (set_speed_mode also clears these). */
    ecgMetResults.denoiseLatMaxUs = 0;
    ecgMetResults.segmentLatMaxUs = 0;
    ecgMetResults.arrhythmiaLatMaxUs = 0;
    /* Measured and projected, never blended (issue #8): `util` above is the
     * measured figure cpu_proj is stated against, and cpu_proj applies the
     * per-stage duty factors (telemetry.h) with capture at 1.0. The coarse
     * breakdown sums to 100 and carries two further terms that are not emitted
     * because they follow from these: idle, and an `other` holding PPG stage
     * time, DSP paths and RTOS overhead, which has no per-stage counters and
     * which cpu_proj deliberately excludes. */
    hkv_log_fx2("cpu_proj", appMetResults.cpuProjPerc);
    hkv_log_fx2("cpu_cap", appMetResults.cpuSplit.capture);
    hkv_log_fx2("cpu_inf", appMetResults.cpuSplit.inference);
    hkv_log_fx2("cpu_tx", appMetResults.cpuSplit.transport);
    /* Read the cached BLE diagnostics; avoid stack scanning in the streaming path. */
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
    hkv_log_u32("ble_hwm", g_cpu_ble_hwm);
    hkv_log_u32("ble_init", (uint32_t)ble_bringup_init_status());
#endif
}

typedef struct {
    const char *subsystem;
    void (*extra)(void);
} hkv_report_line_t;

/* The rotation. Adding a subsystem is one row; the slot period below keeps the
 * rotation at one second on its own. Subsystems with no `extra` are emitted
 * entirely from the obs.h counter and gauge tables. */
static const hkv_report_line_t kReportLines[] = {
    {"sensor", report_extra_sensor},
    {"tio", report_extra_tio},
    {"tiousb", NULL},
    {"txecg", report_extra_txecg},
    {"txppg", report_extra_txppg},
    {"pipe", NULL},
    {"ring", report_extra_ring},
    {"ecgmet", report_extra_ecgmet},
    {"ppgmet", report_extra_ppgmet},
    {"cpu", report_extra_cpu},
};

#define HKV_REPORT_LINE_COUNT (sizeof(kReportLines) / sizeof(kReportLines[0]))

/* One full rotation per second, whatever the list length, so `_ps` stays a
 * per-second rate without anyone having to remember to retune this. */
#define HKV_REPORT_SLOT_MS (1000u / HKV_REPORT_LINE_COUNT)

static_assert(HKV_REPORT_SLOT_MS > 0, "report rotation cannot exceed one subsystem per millisecond");
/* Exact division keeps the report rotation aligned with per-second counter units. */
static_assert(1000u % HKV_REPORT_LINE_COUNT == 0,
              "report line count must divide 1000 ms exactly, or _ps is not a per-second rate");

void
ReportTask(void *pvParameters)
{
    (void)pvParameters;
#if EN_APP_REPORT
    TickType_t lastWake = xTaskGetTickCount();
    size_t idx = 0;

    while (true) {
        /* vTaskDelayUntil, so a long line does not push the rotation late and
         * turn the `_ps` deltas into something other than per-second. */
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(HKV_REPORT_SLOT_MS));
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
        /* Scan the stack outside the logging mutex to avoid blocking other reporters. */
        if (kReportLines[idx].extra == report_extra_cpu) {
            g_cpu_ble_hwm = ble_bringup_radio_stack_free_words();
        }
#endif
        hkv_report_subsystem(kReportLines[idx].subsystem, kReportLines[idx].extra);
        idx = (idx + 1u) % HKV_REPORT_LINE_COUNT;
    }
#else
    /* Counters and gauges keep running; only the printing is off. Keeping the
     * task alive rather than deleting it holds the task count -- and therefore
     * the CPU attribution -- identical across the A/B builds used to measure
     * what the reporting itself costs. */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}

int
main(void)
{
    const uint32_t tioTxQueueDepth = TIO_TX_QUEUE_DEPTH;

    nsx_core_config_t core_cfg = {
        .api = &nsx_core_V1_0_0,
    };
    NSX_TRY(nsx_core_init(&core_cfg), "Core Init failed.\n");

    /* Before any task exists, so every hkv_log_* call from here on is covered.
     * Calls made before this point are still safe: with no mutex yet, the log
     * falls through unlocked, which is correct while main() is the only
     * context running. */
    hkv_log_init();

    sensorCtx.inputSource = appState.inputSource;
    nsxPwrCfg.perf_mode = appState.speedMode ? NSX_POWER_PERF_HIGH : NSX_POWER_PERF_LOW;

    /* Enable ITM/SWO BEFORE nsx_power_configure()/perf-mode switch -- see
     * phase-3 note in git history: enabling SWO requires briefly powering
     * up Crypto to unlock the DCU, and that handshake requests HFRC from
     * the clock manager, which hangs on Apollo5-family secure parts if the
     * CPU has already moved to a SYSPLL-sourced high-performance clock. */
    nsx_itm_printf_enable();

    NSX_TRY(nsx_power_configure(&nsxPwrCfg) != NSX_STATUS_SUCCESS, "Power Init failed.\n");
    /* nsx_power_configure() applied perf_mode, so the core may now be at the
     * high-performance clock while SystemCoreClock still reads the boot value.
     * Fix it here, before vTaskStartScheduler() programs SysTick from it.
     * Issue #25. */
    timebase_sync_to_core_clock();
    nsx_delay_us(200000);

#if AS7058_USE_SPI
    NSX_TRY(nsx_spi_interface_init(&nsxSpiCfg, AM_HAL_IOM_2MHZ, AM_HAL_IOM_SPI_MODE_2) != NSX_STATUS_SUCCESS,
            "SPI Init Failed\n");
#else
    NSX_TRY(nsx_i2c_interface_init(&nsxI2cCfg, AS7058_I2C_SPEED_HZ) != NSX_STATUS_SUCCESS, "I2C Init Failed\n");
#if defined(AM_PART_APOLLO330P)
    /* The generic BSP IOM helper omits the click-specific pins; see #37. */
    NSX_TRY(am_hal_gpio_pinconfig(AM_BSP_GPIO_IOM2_SCL_CB, g_AM_BSP_GPIO_IOM2_SCL_CB) != AM_HAL_STATUS_SUCCESS,
            "Click SCL Init Failed\n");
    NSX_TRY(am_hal_gpio_pinconfig(AM_BSP_GPIO_IOM2_SDA_CB, g_AM_BSP_GPIO_IOM2_SDA_CB) != AM_HAL_STATUS_SUCCESS,
            "Click SDA Init Failed\n");
#endif
#endif

    NSX_TRY(rtos_time_init(), "RTOS Timer Init failed.\n");

    NSX_TRY(sensor_init(&sensorCtx) != ERR_SUCCESS, "Sensor Init failed.\n");

    NSX_TRY(ecg_denoise_init(), "ECG Denoise Init Failed\n");
    NSX_TRY(ecg_segmentation_init(), "ECG Segmentation Init Failed\n");
    NSX_TRY(ecg_arrhythmia_init(), "ECG Arrhythmia Init Failed\n");
    NSX_TRY(metrics_init(&metricsCfg), "Metrics Init Failed\n");

    dwt_init();

    g_tioTxQueue = xQueueCreate(tioTxQueueDepth, TIO_USB_PACKET_LEN);
    NSX_TRY((g_tioTxQueue == NULL), "TIO TX queue create failed\n");

#if TIO_USB_ENABLED
    NSX_TRY(tio_usb_init(&tioUsbCtx) != NSX_STATUS_SUCCESS, "TileIO USB Init failed.\n");
#endif

    /* Initialize radio resources after board power configuration. */
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
    NSX_TRY(ble_bringup_init() != NSX_STATUS_SUCCESS, "TileIO BLE bring-up failed.\n");
#endif

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

    /* Emit build identity before task output can interleave with it. */
    hkv_log_boot();

    /* Constant after model initialization, so once is enough; emitted after the
     * boot line to keep that line first in a capture. */
    hkv_log_begin("model");
    hkv_log_u32("den_arena_used", (uint32_t)ecg_denoise_arena_used());
    hkv_log_u32("den_arena_size", (uint32_t)ecg_denoise_arena_size());
    hkv_log_u32("seg_arena_used", (uint32_t)ecg_segmentation_arena_used());
    hkv_log_u32("seg_arena_size", (uint32_t)ecg_segmentation_arena_size());
    hkv_log_u32("arr_arena_used", (uint32_t)ecg_arrhythmia_arena_used());
    hkv_log_u32("arr_arena_size", (uint32_t)ecg_arrhythmia_arena_size());
    hkv_log_end();

    nsx_freertos_start();

    while (1) {}
}

/* configUSE_MALLOC_FAILED_HOOK == 1 requires this application hook. */
void
vApplicationMallocFailedHook(void)
{
    nsx_printf("heartkit-vitals-demo: malloc failed\r\n");
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}

/* configCHECK_FOR_STACK_OVERFLOW != 0 requires this application hook. */
void
vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    nsx_printf("heartkit-vitals-demo: stack overflow in %s\r\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for (;;) {}
}
