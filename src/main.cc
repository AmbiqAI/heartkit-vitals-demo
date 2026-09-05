// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
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
 * Notes vs legacy (see plan.md phase 6 notes and sensor.h):
 *  - Canned-stimulus patient playback (load_patient_data, sensor.c) is now
 *    implemented: selecting a non-live input source via UIO substitutes the
 *    canned ecg/ppg1/ppg2 stimulus for live AS7058 FIFO data, enabling the
 *    noise-injection and denoise-quality cosine-similarity paths below.
 *  - True dual-wavelength PPG/SpO2: FIXED in this revision. sensor.c
 *    previously hardcoded the simplified single-wavelength JSON-generated
 *    "click_ppg_ecg" profile (one LED, wrong physical LED mapping baked
 *    into the raw JSON data); it now applies the real "click golden"
 *    profile via as7058_get_active_profile() (Red PPG1_SUB1 + IR PPG1_SUB2
 *    + ECG, matching legacy's default), with the LED sub1/sub2 physical
 *    mapping override legacy applies at runtime. PPG signal streaming is
 *    now 2ch and spo2 is a real ratiometric value (pk_ppg math + the
 *    profile's calibration coefficients -- no AMS on-chip bio_spo2_a0
 *    algorithm needed, see sensor.h).
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
#include "metrics.h"
#include "nstdb_noise.h"
#include "obs.h"
#include "ringbuffer.h"
#include "sensor.h"
#include "store.h"
#include "timebase.h"
#include "tio_tx_sm.h"

#include "tflm.h"
#include "ecg_arrhythmia.h"
#include "ecg_denoise.h"
#include "ecg_segmentation.h"

#include "tio_usb.h"

/* TileIO BLE is board-gated to apollo510b_evb (only board in this app's
 * family with the EM9305 BLE radio -- see nsx.yml's `boards:
 * [apollo510b_evb]` scoping of nsx-tileio-ble/nsx-ble/nsx-cordio and
 * CMakeLists.txt's matching `if(NSX_BOARD STREQUAL "apollo510b_evb")`
 * source/link gate). ble_bringup.h/.c are not even compiled in for the
 * other two boards. */
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
#include "ble_bringup.h"
#endif

static TaskHandle_t sensorIrqTaskHandle;
static TaskHandle_t ecgProcessTaskHandle;
static TaskHandle_t ppgProcessTaskHandle;
static TaskHandle_t cpuProcessTaskHandle;
static TaskHandle_t tioProcessTaskHandle;
static TaskHandle_t reportTaskHandle;

/* Capacity of the run-time-stats snapshot buffer, used in BOTH the array
 * declaration and the uxTaskGetSystemState() call -- a named constant rather
 * than two hard-coded 10s that can drift apart, which is how this got close to
 * failing in the first place.
 *
 * WHY THIS IS NOT A TUNING KNOB. uxTaskGetSystemState() returns 0 -- not a
 * truncated list -- when the array cannot hold every task. The previous value
 * was 10 against 9 live tasks on the BLE build (6 app + idle + timer + BLE
 * radio), so ONE additional task anywhere, including one created inside a
 * vendored module, would have silently zeroed every per-task percentage and
 * pinned the reported CPU utilisation at a constant value with no other
 * symptom. 16 restores real headroom, and CpuProcessTask now counts the
 * overflow (HKV_CNT_CPU_STAT_OVERFLOW) and skips the interval instead of
 * publishing a number computed from an empty list. The `tasks` field on the
 * `cpu` report line shows how much of this is actually in use. */
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
    /* Correct as long as SystemCoreClock is truthful, which timebase_sync_to_
     * core_clock() keeps it (issue #25). An inference that spans a speed-mode
     * switch is scaled by whichever value is live when it FINISHES, so that
     * one measurement is wrong; the next one is right. Accepted. */
    return deltaCycles / (SystemCoreClock / 1000000);
}

/* Legacy IPS scale: 2e6/deltaUs (legacy main.cc used 2000000.0/deltaUs with a
 * true-microsecond ticker) -- the host dashboard expects this scale. Guarded
 * against deltaUs==0 (fast DSP paths + coarse cycle->us division). */
static inline float32_t
ips_from_delta_us(uint32_t deltaUs)
{
    return 2.0e6f / (float32_t)MAX(deltaUs, 1u);
}

/* Inference power at a given operating point, in mW. THE ONLY PLACE the LP/HP
 * inference constants are selected between: the three IPS/W tiles and the
 * battery model both call this, so the pair in constants.h is never duplicated
 * at a use site. HP is measurably LESS efficient per inference than LP
 * (16.7 vs 5.5 mW, #18 runlogs), so a tile pinned to the LP figure over-states
 * HP efficiency -- see issue #25 AC5.
 *
 * TAKES THE MODE, does not read it. appState.speedMode is a live dashboard
 * control (TIO_UIO_SPEED_MODE_IDX -> set_speed_mode ->
 * nsx_power_set_performance_mode) and can change between any two reads, so a
 * caller that also needs the mode for something else (CpuProcessTask needs it
 * for compute power too) must read it ONCE and pass the same value here.
 * Otherwise one published window can mix an LP compute figure with an HP
 * inference figure. The uint8_t read itself needs no lock -- single aligned
 * byte -- and the worst case is a tile that is one sample stale. */
static inline float32_t
inference_power_mw(bool hpMode)
{
    return hpMode ? (float32_t)MCU_INFERENCE_POWER_MW_HP : (float32_t)MCU_INFERENCE_POWER_MW_LP;
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
// NSX-specific, so it needs no additional wrapper.

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
    /* nsx-usb encodes WebUSB scheme 1 (HTTPS) separately, so this must be
     * the host/path only, not a full URL. */
    .webusb_url = "ambiqai.github.io/tileio/",
    .vid = TIO_USB_VENDOR_ID,
    .pid = TIO_USB_PRODUCT_ID,
};

/*
 * Pipeline flush on host (re)connect, race-free version.
 *
 * ringbuffer.c is not thread-safe: each rb tolerates exactly one producer
 * (head writer) and one consumer (tail writer). ringbuffer_flush() writes
 * tail, so it may only be executed by each buffer's CONSUMER task -- a
 * cross-task flush racing a concurrent seek/pop can leave tail past head,
 * making ringbuffer_len() report a huge bogus length (this hazard existed
 * in legacy too). Instead of flushing directly, TioProcessTask raises
 * per-owner request flags and each owning task flushes its own buffers at
 * the top of its loop.
 */
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
            /* Tell the newly connected host our current mode state so its UI
             * reflects reality without requiring it to write UIO first --
             * matches legacy's check_webusb_state(). (Runs in TioProcessTask
             * context; send_uio_state only enqueues.) */
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
        /* The core clock just changed under the software timebase. Without
         * this the FreeRTOS tick keeps its old period in CPU cycles and runs
         * fast (or slow) by the clock ratio, and every dwt_delta_us() is
         * scaled by a stale SystemCoreClock. Issue #25. */
        timebase_sync_to_core_clock();
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
    /* Enqueue (slot 0, type 2 = UIO) via the ISR-safe TX queue rather than
     * calling tio_usb_send_uio_state() directly -- the direct call blocks in
     * retry loops, which is unacceptable from any context TioProcessTask
     * shares with time-critical work (and fatal from ISR context; see
     * received_uio_state below). Matches legacy's send_uio_state(). */
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
/*
 * ble_bringup.c (a plain C file) needs to hand these callbacks to
 * tio_ble_context_t as C function pointers, but received_slot_data/
 * received_uio_state above are C++-linkage `static` functions -- neither
 * `extern`-able by name (static) nor C-callable without extern "C" (name
 * mangling). Rather than changing their linkage/visibility (and risking the
 * hardware-validated USB callback wiring above), add two tiny extern "C"
 * forwarders with matching signatures that main() hands to ble_bringup_init()
 * indirectly via ble_bringup.c's tio_ble_context_t.
 */
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

/*
 * ROOT-CAUSE NOTE (AS7058 ISR freeze on host connect): nsx-tileio-usb
 * dispatches this callback from the NSX_TIMER_USB timer *ISR* (its vendor
 * RX handler runs inside usb_timer_callback). An earlier revision applied
 * the UIO state and called send_uio_state() -> tio_usb_send_uio_state()
 * directly here -- a blocking send that can spin in am_util_delay_ms(1)
 * retry loops (plus perf-mode switching and printfs), all in ISR context.
 * That stalls same/lower-priority IRQs long enough to blow the AS7058's
 * bounded INT-service window; with its edge-triggered INT line stuck high
 * and never re-armed, the sensor stops interrupting permanently -- observed
 * as isr/push counters freezing at the exact "host connected" moment.
 *
 * Fix: do nothing here but latch a state update or state-request flag;
 * TioProcessTask applies settings and enqueues replies in task context.
 */
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

///////////////////////////////////////////////////////////////////////////////
// Rate-matched signal emission
///////////////////////////////////////////////////////////////////////////////
//
// See constants.h "TileIO latency budget" and issue #12.
//
// Every pump tick, per signal slot:
//
//   1. TRIM the slot's TX rings down to their high-water H, discarding the
//      OLDEST samples and keeping the newest H. Anything above H is stale
//      backlog we have already decided not to ship (no catch-up, ever). The
//      discarded sample count is accumulated per slot so it can later be
//      signalled to the host as a sequence discontinuity (TimedSignal v2
//      carries a per-slot sample sequence; tracked separately). For now it is
//      exposed as a counter -- see the trim acceptance rate in constants.h,
//      which is a small drift allowance rather than a hard zero.
//   2. Emit AT MOST ONE packet, of TIO_*_SAMPLES_PER_PKT samples, plus
//      TIO_TX_DRIFT_CATCHUP_SAMPLES more while the trough servo still has
//      budget for this window. If a full packet is not available, emit
//      nothing this tick rather than a short packet.
//
//      The nominal count assumes the producer runs at exactly the pump's
//      nominal rate; it does not. The two corrections -- skip a tick when
//      short, pop one extra when long -- let the pump track the producer's
//      real rate in both directions. Neither can push emission above 1x,
//      because the pop comes out of a ring and can only return samples the
//      producer already produced; that invariant is structural, not a
//      property of the count. See constants.h.
//
// SAMPLE ALIGNMENT (the part that silently corrupts the waveform if it is
// wrong): a slot's TX taps are several parallel rings holding the same sample
// index in each position (ECG: raw/denoised/mask; PPG: red/IR). They only stay
// aligned if every ring is advanced by the SAME number of samples on every
// operation. That is enforced structurally here rather than by convention:
//
//   * The rings of a slot are held in one group, and all counts are derived
//     from tio_tx_group_avail(), the MINIMUM length across the group.
//   * The trim seeks every ring in the group by one single `drop` value
//     computed once, in one loop.
//   * The pop loop below is bounded by one single `numSamples` value returned
//     by tio_tx_group_prepare(), and pops exactly one sample from each ring
//     per iteration.
//
// There is deliberately no code path that advances one ring of a group without
// advancing the others by the same amount.

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
    /* Observability IDs, not storage. The values live in the obs.h counter and
     * gauge tables so that every counter in the app is emitted by one
     * table-driven reporter and adding one needs no reporter or parser change.
     * The group only carries which row it owns.
     *
     * cntPumped counts ticks on which a full packet was assembled and handed
     * to the transport; cntDelivered counts those the transport accepted. They
     * are NOT the same number and the difference matters: samples are popped
     * from the rings before the enqueue result is known, so during a host
     * blackout the rings still drain and pump_ps keeps reading 10/s with
     * trim_ps at 0 while nothing reaches the host. pump_ps == deliv_ps is the
     * healthy case; a gap between them is real loss, corroborated by the
     * `tio` and `tiousb` report lines.
     *
     * cntDrained counts ticks on which the servo spent an extra sample.
     * Compare against extraBudget: if it tracks the budget the servo is in
     * control, and if it falls short the spend guard is holding the pump off a
     * near-empty ring. MIND THE UNITS -- drain_ps is per second while bgt is
     * per servo window (64 ticks = 6.4 s).
     *
     * gaugeOcc is the occupancy watermark pair, sampled pre-trim and pre-pop
     * so it is the true peak the producer created including whatever the trim
     * is about to discard. Windowed, so the report shows a progression rather
     * than a lifetime extreme. For a block-structured producer like ECG the
     * low value IS the pre-block residual R: the sawtooth trough is the last
     * pump tick before the next block lands. */
    hkv_counter_id_t cntTrimmed;   /* samples discarded by trim-to-high-water */
    hkv_counter_id_t cntPumped;
    hkv_counter_id_t cntDelivered;
    hkv_counter_id_t cntDrained;
    hkv_gauge_id_t gaugeOcc;
} tio_tx_group_t;

#define TIO_OCC_MIN_INIT (0xFFFFFFFFu)

/* Designated initialisers deliberately, not positional. These are 16 fields of
 * which most are same-typed zeros, the build does not enable -Wextra, so
 * -Wmissing-field-initializers is off, and a reorder would not warn. The
 * specific hazard: if troughTarget silently took another field's zero, the
 * spend guard degrades to `avail > samplesPerPkt` and the servo takes an extra
 * sample on nearly every tick.
 *
 * The same argument now covers the counter/gauge ids: they are consecutive
 * enum values, so a positional mix-up would attribute PPG trim to the ECG
 * report line and nothing would warn. Naming each one is the check. */
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

/* Peak ring occupancy is not H: the trim runs BEFORE the pop, so the producer
 * can land a full structural block on top of (H - samplesPerPkt) samples that
 * survived the previous tick. Assert on that, not on H alone -- asserting
 * H < BUF_LEN would still pass if a window constant grew enough to overrun.
 *
 * The drift drain does not enter this bound: popping the extra sample only
 * ever leaves FEWER samples behind, so the worst case is still the tick that
 * pops the nominal count.
 *
 * Headroom as configured: ECG 280 - 10 + 200 = 470 against ECG_TX_BUF_LEN 500,
 * leaving 30 samples (300 ms). PPG 93 - 10 + 13 = 96 against PPG_TX_BUF_LEN
 * 500, leaving 404. The ECG figure is the one to watch -- raising H or the
 * segmentation window eats it directly, and at H = 310 it is gone. */
static_assert(TIO_ECG_TX_HIGH_WATER - TIO_ECG_SAMPLES_PER_PKT + TIO_ECG_TX_BLOCK_SAMPLES <= ECG_TX_BUF_LEN,
              "ECG TX peak occupancy (H - pkt + block) exceeds ring capacity");
static_assert(TIO_PPG_TX_HIGH_WATER - TIO_PPG_SAMPLES_PER_PKT + TIO_PPG_TX_BLOCK_SAMPLES <= PPG_TX_BUF_LEN,
              "PPG TX peak occupancy (H - pkt + block) exceeds ring capacity");

/* A full structural block landing on the trough must still fit under H, or the
 * trim fires on the very next tick and discards fresh signal.
 *
 * FIVE terms, because the trough is not the target and the block is not
 * punctual: the trough settles up to PULL_DIV above target (the position
 * term's deadband), alternates a further samplesPerPkt with the block-period
 * quantisation, and the block can arrive TIO_TX_SLIP_SAMPLES late because
 * EcgProcessTask runs one mutually-exclusive stage per iteration and a due
 * segmentation is deferrable.
 *
 * This bound has been wrong twice by omitting a term. The two-term form passed
 * at a target of 30 while the real peak sat at 248 of 250. The four-term form
 * ignored producer scheduling and claimed ~41 samples of margin where
 * simulation showed 3 -- one more iteration of slip and it trimmed. Add terms
 * here rather than simplifying; see TIO_TX_SLIP_SAMPLES for the measurements. */
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

/* The servo window must span at least THREE whole producer blocks.
 *
 * One block is not enough, which cost a bench iteration to learn. The block
 * period is a non-integer number of pump ticks, so the true trough alternates
 * by ~samplesPerPkt from block to block; a window spanning 1-2 blocks catches
 * that alternation in its minimum and the servo's rate term differentiates the
 * artifact rather than the drift. Three blocks guarantees the minimum is taken
 * over enough troughs to land consistently at the bottom of the alternation.
 * ECG is the binding case: block/samplesPerPkt ticks to drain one block. */
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

/* Trough servo. Runs once per servo window, not per tick.
 *
 * WHAT IT CORRECTS. The producer and the pump are on different clocks, so the
 * producer delivers 100 +/- a few tenths of a percent samples per second while
 * the pump nominally takes exactly 100. The residual is a standing RATE error
 * of a couple of samples per second, and correcting a rate error is the job
 * here -- not correcting instantaneous occupancy.
 *
 * WHY A THRESHOLD COULD NOT DO IT. The previous design popped an extra sample
 * on every tick whose occupancy exceeded a setpoint. That asks one number to
 * be two things at once: the target trough (which must satisfy
 * trough + block <= H, so <= 50) and the drain trigger (which for a block
 * producer must sit near the peak, ~200, so that only the few ticks per block
 * actually needed qualify). For ECG those are incompatible, and measurement
 * confirmed both failure modes -- setpoint 240 under-drained and trimmed
 * ~1.8 samples/s forever; setpoint 50 over-drained, emptied the ring and
 * produced whole seconds with no ECG packet at all.
 *
 * HOW THIS ONE WORKS. Each window, compare the measured trough against the
 * target and nudge a BUDGET of extra samples by one. The budget is the rate
 * correction; the trough is the error signal. The pump then spends at most
 * that many extra samples over the following window, at most one per tick, and
 * only while occupancy is comfortably above target. Corrections are therefore
 * bounded by construction and the trough is held near target instead of being
 * driven to either rail.
 *
 * CONTROL LAW, and why it is not simply "nudge the budget toward the target".
 * The plant is an integrator: the budget sets a RATE, and the trough is the
 * accumulated position. Driving an integrator with a position error alone
 * (budget += step when the trough is high) is a double integrator and it limit
 * cycles -- simulated against the measured drift it swings the trough 0 -> 64
 * -> 0 with an ~80 s period, which would trim at the peak and skip at the
 * floor. So the law has two terms:
 *
 *   budget += (trough - trough_prev)          rate term
 *           + (trough - target) / PULL_DIV    position term
 *
 * The rate term is the whole correction and it is dead-beat: the trough moved
 * by exactly the imbalance between production and consumption over the window,
 * so adding that difference to the budget cancels the drift in ONE window and
 * leaves the trough wherever it sits. The position term is what then walks the
 * trough back to target, gently, over a first-order tail of ~PULL_DIV windows.
 * Integer division gives it a natural deadband: inside +/-PULL_DIV samples of
 * target it contributes zero and the servo holds still.
 *
 * STABILITY, which matters more here than convergence speed:
 *  - The error signal is the per-window MINIMUM, and the window is asserted to
 *    span at least three producer blocks. So the servo sees the trough, not
 *    the 2 s sawtooth whose 200-sample swing would otherwise swamp the ~2/s
 *    drift it is trying to measure.
 *  - The budget is clamped to [0, TIO_TX_SERVO_MAX_BUDGET], so there is no
 *    windup: a producer that stops entirely parks the budget at 0 rather than
 *    accumulating a debt to spend later as a burst.
 *  - It responds from both directions. Producer fast: trough rises, budget
 *    rises, extra draining. Producer slow: trough falls, budget falls to 0 and
 *    the pump self-throttles by skipping, which is the correct response since
 *    samples cannot be manufactured.
 *
 * Those are the properties the law was designed for, and they hold. What does
 * NOT hold is convergence to a stable operating point -- an earlier version of
 * this comment claimed a monotone approach to target, and hardware disproved
 * it. See RESIDUAL BEHAVIOUR below before trusting any stability claim here.
 *
 * RESIDUAL BEHAVIOUR: THE SERVO DOES NOT CONVERGE. Read this before tuning it.
 *
 * It holds the stream inside its acceptance envelope, but it does not settle
 * on a stable budget. Measured on hardware 2026-09-01, ECG, over a 107 s
 * window taken from 60 s into the run -- comfortably past the ~12.7 s the
 * budget needs to become useful, and there is no later settling point to wait
 * for since the dither never stops:
 *
 *   bgt   0 x13, 7 x6, 9 x4, 10 x31, 13 x7, 19 x7, 20 x26, 31 x13
 *         -- a 0..31 spread clustering near multiples of samplesPerPkt,
 *            not the stable ~15 the design intends.
 *   trgh  3..34 spread, against a target of 20.
 *
 * WHY IT IS SHIPPED ANYWAY. Every quantity that matters is comfortably inside
 * bounds, and the dither is a fraction of a sample per second of rate error
 * that never accumulates:
 *
 *   trim  0/s on every line -- no sample is ever discarded.
 *   pkt   mean 9.96/s, no interval below 9 -- no emission gap.
 *   occ   observed peak ~209 against H = 250, ~41 samples of real margin
 *         (the four-term static_assert above bounds the theoretical worst
 *         case at 238, and the observed peak sits well under even that).
 *
 * The controlled variable misbehaving while every controlled OUTCOME is in
 * spec means the loop is sloppy, not unsafe. It was not worth further tuning
 * passes against a bench.
 *
 * WHAT DID NOT FIX IT, so nobody re-derives a false premise. The block-period
 * aliasing diagnosis at TIO_TX_SERVO_WINDOW_TICKS is real -- the block period
 * genuinely is a non-integer 19.53 pump ticks and the trough genuinely does
 * alternate by a packet -- but widening the window from 32 to 64 ticks did NOT
 * remove the dither. It WIDENED it: the earlier 32-tick window gave bgt 0<->10
 * and trgh 13<->23, tighter than the 0..31 and 3..34 above. So aliasing is at
 * most part of the cause. The likely reason widening failed is that 64 ticks
 * is 3.28 block periods -- still not an integer multiple, so the number of
 * troughs captured per window and their phases keep changing, and the
 * per-window minimum keeps stepping by packet-sized amounts. A fixed-length
 * window cannot be an integer multiple of a block period that is set by an
 * independent, drifting producer clock.
 *
 * A SECOND MECHANISM, found by simulation and not by the window analysis. The
 * rate term cannot distinguish producer SCHEDULING SLIP from clock drift: a
 * block deferred by an iteration looks exactly like a producer that briefly
 * sped up, so the servo corrects for a rate change that never happened. The
 * asymmetry is what makes it stick -- when the block arrives late the trough
 * dips and the correction goes negative, but the 0-clamp rectifies it, so the
 * budget is not given back on the rebound. Simulated with perfectly periodic
 * blocks the dither is only bgt 7..20 / trgh 11..24; adding one iteration of
 * slip reproduces the measured 0..31 / 3..34 almost exactly. There is also a
 * boot transient: the first window measures a trough of 0 while the ring is
 * still filling, that value becomes servoTroughPrev, and the next window's
 * rate term is the entire fill transient.
 *
 * So the dither has two causes, not one, and neither is a gain problem --
 * which is why no amount of gain tuning fixed it.
 *
 * WHAT TO INVESTIGATE NEXT, if a future maintainer wants real convergence: do
 * not tune the gains -- make the MEASUREMENT synchronous with the producer.
 * Detect the block push (an occupancy jump of ~block samples) and latch the
 * trough once per block rather than once per fixed window. That removes the
 * phase beat at its source rather than averaging over it, it makes the rate
 * term a true per-block imbalance, and it addresses the slip mechanism too --
 * a block measured per block is not "late" relative to its own arrival, so
 * scheduling deferral stops masquerading as drift. One change, both causes.
 * It is a structural change to the error signal, which is why it was out of
 * scope here.
 *
 * ALSO RECORDED: bgt was observed at 31 against TIO_TX_SERVO_MAX_BUDGET of 32
 * on 13 of those lines. It is not pinned, but it is close, so if someone later
 * finds it sitting at the cap they should know it was already reaching 31
 * intermittently at ~2.4% producer drift -- and that the cap is what stops the
 * budget becoming a burst, so raising it is not automatically the right move.
 *
 * Two smaller, understood contributors to the offset, both benign: the trough
 * settles ABOVE target by up to PULL_DIV because the position term's integer
 * division has no restoring force inside its deadband, and the underlying
 * occupancy alternates by ~samplesPerPkt from the block quantisation. Both are
 * carried in the peak static_assert above. */
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
    /* Spend the servo's budget: at most one extra sample per tick, at most
     * extraBudget per window, and only while occupancy stays a full packet
     * clear of the target trough.
     *
     * That guard is what stops the over-draining that emptied the ring under
     * the old threshold design: however large the budget, the pump stops
     * taking extra as soon as occupancy approaches target, so the correction
     * can never pull the trough down to where the next tick has to skip.
     *
     * The 1x invariant remains structural rather than tuned: the pop comes out
     * of a ring, so it can only ever return samples the producer has already
     * produced. */
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

/* Pace a signal pump task at exactly TIO_PUMP_INTERVAL_MS.
 *
 * vTaskDelayUntil() (not vTaskDelay) so the period is measured from the
 * previous wake time and scheduling latency does not accumulate into drift --
 * the old "measure the loop with DWT, then vTaskDelay the remainder" form lost
 * the measurement/delay gap on every single iteration. At
 * configTICK_RATE_HZ = 1000, pdMS_TO_TICKS(100) is exactly 100 ticks.
 *
 * Overrun handling is the subtle part, and it has to hit `max(work, period)`
 * exactly -- both neighbouring behaviours are bugs:
 *
 *   * Letting vTaskDelayUntil() fire back-to-back to catch up emits packets
 *     closer together than the period, i.e. ABOVE 1.00x realtime, which the
 *     latency budget forbids.
 *   * Re-anchoring to `now` and then delaying is worse in the other
 *     direction: the deadline `now + period` is still in the future, so the
 *     task blocks a further full period and the iteration costs `work +
 *     period`. That runs the pump BELOW 1x, and because the producer is
 *     clocked independently the resulting sample deficit accumulates until
 *     the trim starts discarding it -- a recurring splice, exactly the
 *     artifact this file exists to remove. Overrun is a real condition here
 *     (one long inference per cycle is enough), not a hypothetical.
 *
 * So on overrun, re-anchor to `now - period`: vTaskDelayUntil() then sees a
 * deadline of `now`, returns immediately without blocking, and leaves
 * *pLastWake == now for the next cycle. The period is dropped, never
 * compressed and never doubled.
 *
 * Note for bench runs: enabling the debug log flags puts blocking SWO writes
 * into these equal-priority tasks and makes overrun materially more likely --
 * the instrumented build is the one most likely to exercise this path. */
static void
tio_pump_wait(TickType_t *pLastWake)
{
    const TickType_t period = pdMS_TO_TICKS(TIO_PUMP_INTERVAL_MS);
    TickType_t now = xTaskGetTickCount();
    /* SIGNED delta. An unsigned compare treats a deliberately future-dated
     * anchor as a huge positive elapsed time and fires the overrun branch,
     * which silently discarded the PPG phase stagger: PpgProcessTask anchors
     * at T+33, the first iteration reaches here at ~T+20, and the unsigned
     * (now - *pLastWake) underflows to a value comfortably >= period. Signed
     * arithmetic reads that as -13 ticks (not yet due) and leaves the anchor
     * alone, while remaining wrap-safe for the same reason the unsigned form
     * was: the difference is what wraps, not the operands. */
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

/* Per-stage non-zero-return counts live in the obs.h counter table
 * (HKV_CNT_PIPE_ERR_*), so they are ALWAYS compiled in, unlike the
 * EN_APP_TRACE lines they sit beside. With tracing off -- the default -- the
 * inference return codes are otherwise discarded at the `(void)err`, and a
 * persistently failing denoise or segmentation stage would produce a
 * plausible-looking flat trace with no indication anywhere that the model
 * never ran. Two increments per 2 s: the counters are not what costs, the
 * printing is. */

void
EcgProcessTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t err = 0;
    uint32_t tickStart;
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

            ecgMetResults.denoiseIps = ips_from_delta_us(dwt_delta_us(tickStart));
            /* Publish the run counter AFTER the duration it belongs to, never
             * before. CpuProcessTask runs in a different task and pairs this
             * stage's runsDelta with its *Ips to derive inference duty; with
             * the count first, a window sampled mid-execution sees a run whose
             * duration has not been written yet and bills it at the previous
             * value. On the first-ever run that value is the store.c seed
             * (1.0), which is a legitimate ips (a 2 s stage) and so cannot be
             * screened out downstream as a sentinel.
             *
             * SOURCE ADJACENCY IS NOT ENOUGH, hence the barrier. `*Ips` is an
             * ordinary non-volatile store and the counter increment inside
             * hkv_count() is volatile; C does not order non-volatile accesses
             * against volatile ones, so the compiler is free to sink the store
             * past the increment. GCC 15.2 was measured doing exactly that at
             * the metrics site: recompiling this file with the three barriers
             * removed puts MET_RUNS (g_hkv_counters+140) at EcgProcessTask
             * +0x528 and the arrhythmiaIps store (ecgMetResults+24) at +0x532,
             * i.e. counter first. With the barriers the same store lands at
             * +0x4f4, ahead of the counter at +0x52e. The empty asm with a
             * "memory" clobber is what forces that. Same pattern in the
             * segmentation and metrics branches; do not remove it to "tidy up".
             *
             * READING THE DISASSEMBLY -- CHECK THE FIELD OFFSET. Each branch
             * stores TWO floats into ecgMetResults: the *Ips field (+16 den,
             * +20 seg, +24 arr) and the *uIpspw field (+32, +36, +40). Only
             * the *Ips store is ordering-critical. The *uIpspw store is
             * expected to sit after the counter and its position means
             * nothing. Comparing the counter against the +40 store instead of
             * the +24 store makes a correct metrics site look inverted. */
            __asm volatile("" ::: "memory");
            hkv_count(HKV_CNT_PIPE_DEN_RUNS);
            /* DASHBOARD CHANGE: the uIps/W divisor is now the sourced
             * inference power for the CURRENT operating point (5.5 mW LP,
             * 16.7 mW HP) where it used to be AVG_INFERENCE_POWER (7.87 mW,
             * unsourced), so in LP all three *uIpspw values read ~43% higher
             * than on any earlier build. Efficiency did not change; the power
             * figure divided into it did. These now follow appState.speedMode
             * the same way the battery model below does -- see
             * inference_power_mw() and issue #25 AC5. */
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

            if (appState.segMode == SegmentationModeDsp) {
                /* Use the shared physiokit DSP segmentation (same as legacy)
                 * rather than an inline reimplementation: it also stamps the
                 * QoS bits into the mask and reports a qos value, which the
                 * inline version was silently dropping (stale qos in DSP
                 * mode). */
                err = ecg_physiokit_segmentation_inference(ecgSegInout, ecgSegMask, 0, &ecgMetResults.qos);
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

            ecgMetResults.segmentIps = ips_from_delta_us(dwt_delta_us(tickStart));
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

            ecgMetResults.arrhythmiaIps = ips_from_delta_us(dwt_delta_us(tickStart));
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
            HKV_TRACE_KV("ecg", "met_err", err);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        (void)err;

        /* Wait BEFORE sending, not after. With the send ahead of the wait, a
         * long iteration (a 250 ms segmentation tick, say) emits at T+250, the
         * wait correctly returns immediately, and the next near-idle iteration
         * emits again ~2 ms later -- two packets 2 ms apart, a one-packet
         * catch-up the design says never happens. Sending immediately after
         * the wake instead makes inter-packet spacing equal the wake-to-wake
         * interval, max(work, period), by construction. */
        tio_pump_wait(&pumpLastWake);
        send_ecg_signals();
    }
}

///////////////////////////////////////////////////////////////////////////////
// PPG process task
///////////////////////////////////////////////////////////////////////////////
//
// Dual-wavelength pipeline: downsample Red (PPG1_SUB1) + IR (PPG1_SUB2) ->
// metrics (PR/QoS/real ratiometric SpO2 via pk_ppg + sensor_get_spo2_config()
// calibration coefficients) -> TileIO TX (2ch). No denoise/segmentation
// stage, matching legacy's pass-through behavior absent an AI model for PPG.
// (Phase 6 fix: previously ran single-wavelength only because sensor.c
// applied the wrong AS7058 profile -- see sensor.c/store.h.)

/* Loop iterations and teed samples are counted in the obs.h table
 * (HKV_CNT_PIPE_PPG_ITERS / HKV_CNT_PIPE_PPG_PUSHED), and the size of a single
 * tee pass -- the OBSERVED structural block -- is the HKV_GAUGE_PPG_TEE gauge.
 *
 * TIO_PPG_TX_BLOCK_SAMPLES (13) is an empirical figure taken from the AS7058
 * watermark interval, not a compile-time bound: with PPG_DS_RATE == 1 the loop
 * is bounded only by MIN(len(rbPpg1Sensor), len(rbPpg2Sensor)), so a delayed
 * task could in principle tee up to SENSOR_BUF_LEN-1 samples in one pass and
 * exceed H. Unlike ECG, whose block is ECG_SEG_VALID_LEN and therefore
 * statically checkable, this one has to be watched at runtime. That is why the
 * gauge is declared LIFETIME rather than windowed: a single excursion
 * invalidates the derivation of H, and a per-second maximum would scroll it
 * out of the capture. If `tee_hi` on the `pipe` report line exceeds
 * TIO_PPG_TX_BLOCK_SAMPLES, that constant is wrong and H must be re-derived
 * from the real bound. */

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

///////////////////////////////////////////////////////////////////////////////
// CPU utilization task
///////////////////////////////////////////////////////////////////////////////
//
// Reads FreeRTOS per-task run-time counters (configGENERATE_RUN_TIME_STATS,
// FreeRTOSConfig.h) to compute ECG/PPG task CPU utilization, a 30s rolling
// overall utilization average, and a battery-life estimate -- ported from
// legacy's CpuProcessTask.

///////////////////////////////////////////////////////////////////////////////
// Battery model -- three MCU states (issue #17)
///////////////////////////////////////////////////////////////////////////////
//
// MCU energy only, sensor excluded. Scope, sources, the margin and the
// deployment-projection caveat on the sleep term are all documented beside the
// constants in constants.h; read that header before changing anything here.
//
//   idleFrac      = 1 - busy
//   inferenceFrac = sum over stages of (duration_i x runRate_i), clamped <= busy
//   computeFrac   = busy - inferenceFrac, clamped >= 0
//   avgPower_mW   = (inf x INFERENCE + cmp x COMPUTE + idle x SLEEP) / MARGIN
//   batteryDays   = BATT_POWER_CAP / avgPower_mW / 24
//
// `busy` is cpuPercUtil, the MEASURED 30 s rolling utilisation, unchanged by
// this model. It includes demo transport, so it is conservative and needs no
// duty-cycle argument.
//
// THE MODEL FOLLOWS THE OPERATING POINT. INFERENCE and COMPUTE are read per
// window from the LP/HP constant pair that matches `appState.speedMode`, the
// dashboard control that calls nsx_power_set_performance_mode() at runtime.
// Without that, switching to high performance would REPORT more battery life:
// the same work finishes ~2.6x faster, so both `busy` and the derived
// inference duty fall, while the real draw is ~3x higher. SLEEP is shared
// between the two -- Sleep 1 does not depend on the run clock.

/**
 * @brief Wall-time fraction one pipeline stage spent running, over a window.
 *
 * Both inputs are already measured, so nothing here is assumed: `ips` is the
 * stage's last DWT-timed duration in the legacy 2e6/deltaUs scale (see
 * ips_from_delta_us), which inverts to duration_s = 2/ips; `runsDelta` is that
 * stage's own run counter over `windowSec`, so the cadence is derived rather
 * than hardcoded at the nominal 2 s. A stage that stalls drops out of the sum
 * by itself, because its run counter stops advancing.
 *
 * A stage switched to DSP or OFF does NOT drop out. Its hkv_count() still
 * fires, and metrics_capture_ecg() runs unconditionally in the metrics branch,
 * so the stage keeps reporting a (much shorter) DWT duration and that time is
 * billed at inference power. This OVER-bills DSP and off modes, which is the
 * conservative direction, and it is deliberate: the counters stay a
 * measurement of what the pipeline actually did rather than a function of the
 * mode flags. Do not gate the counters on mode to "fix" this.
 *
 * Returns 0 whenever the stage did not run in this window. The ips <= 0 guard
 * is defensive only: store.c seeds all three *Ips at 1.0, so it never fires in
 * practice, and 1.0 is a legitimate ips (a 2 s stage) and cannot be used as a
 * cold-start sentinel. Cold start is handled instead by publishing each run
 * counter AFTER its duration, so runsDelta cannot count a run whose duration
 * has not been written yet. That ordering is NOT a property of the source
 * layout: the *Ips stores are non-volatile and the counter increments are
 * volatile, which C does not order against each other, and GCC was measured
 * reordering one of the three. It is enforced by an explicit
 * `__asm volatile("" ::: "memory")` barrier at each of the three sites in
 * EcgProcessTask. Removing a barrier reintroduces the poisoned first sample.
 *
 * Caveat worth knowing: `ips` is the LAST duration, not the window mean, so a
 * stage whose cost varies is billed at its most recent cost. The 30 s rolling
 * average downstream absorbs most of that.
 */
static inline float32_t
stage_duty_frac(uint32_t runsDelta, float32_t ips, float32_t windowSec)
{
    if (runsDelta == 0u || ips <= 0.0f || windowSec <= 0.0f) {
        return 0.0f;
    }
    return (2.0f / ips) * ((float32_t)runsDelta / windowSec);
}

void
CpuProcessTask(void *pvParameters)
{
    (void)pvParameters;
    const uint32_t samplesPerPublish = kCpuStatsPublishPeriodMs / kCpuStatsSamplePeriodMs;
    /* Elapsed, not nominal. The sample loop is paced by a RELATIVE vTaskDelay,
     * so each iteration takes the delay plus its own work and the real window
     * is always longer than samplesPerPublish x kCpuStatsSamplePeriodMs. Using
     * the nominal value as the duty denominator would understate the window and
     * therefore OVERSTATE inference duty. Tick deltas remove the argument: the
     * denominator is the window that actually elapsed. Re-baselined together
     * with the run counters on every discarded interval below, so the window
     * and the counter deltas always describe the same span. */
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
    uint32_t prevDenRuns = g_hkv_counters[HKV_CNT_PIPE_DEN_RUNS];
    uint32_t prevSegRuns = g_hkv_counters[HKV_CNT_PIPE_SEG_RUNS];
    uint32_t prevMetRuns = g_hkv_counters[HKV_CNT_PIPE_MET_RUNS];
    uint32_t runTimeTicks = 0;
    float32_t ecgTaskPerc = 0, ppgTaskPerc = 0, totalTaskPerc = 0;
    uint32_t prevRun = 0, prevEcg = 0, prevPpg = 0, prevIdle = 0;
    uint32_t runDelta, ecgDelta, ppgDelta, idleDelta;
    size_t numTasks;

    while (true) {
        uint32_t idleCounter = 0;
        service_cpu_flush_request();
        numTasks = uxTaskGetSystemState(xTaskDetails, HKV_TASK_STATUS_CAPACITY, &runTimeTicks);
        if (numTasks == 0) {
            /* Not "no tasks" -- uxTaskGetSystemState() returns 0, rather than
             * a truncated list, when the array is too small for the live task
             * count. Falling through would compute every percentage from an
             * empty list: idleDelta underflows, cpuIdlePerc clamps, and the
             * published utilisation becomes a plausible constant that nothing
             * else contradicts. Count it, skip the interval, and leave the
             * previous published value alone. If stat_overflow is climbing on
             * the `cpu` report line, raise HKV_TASK_STATUS_CAPACITY. */
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

            /* Inference duty for this window: each stage's own measured run
             * rate x its own measured duration. Counters are free-running and
             * never reset by the reporter, so a local delta is the window. */
            /* TIMEBASE -- THE TWO TERMS MUST STAY ON THE SAME CLOCK.
             * Both sides of the duty ratio are derived from SystemCoreClock,
             * and that is what makes this correct in HP mode rather than a
             * coincidence worth preserving deliberately:
             *   - the numerator, 2/ips, comes from dwt_delta_us() (main.cc
             *     :152), which divides DWT cycles by SystemCoreClock;
             *   - the denominator comes from the FreeRTOS tick, and
             *     configCPU_CLOCK_HZ is also SystemCoreClock.
             * HISTORY, and why the invariant is still worth stating. Before
             * issue #25 nothing updated SystemCoreClock on a performance-mode
             * change, so it stayed at 96 MHz while the core ran at 250 MHz: in
             * HP mode the measured durations AND this window were both
             * over-reported by the same 250/96 = 2.604x and the factors
             * CANCELLED, which is the only reason the duty was right. Issue
             * #25 removed the need for that cancellation --
             * timebase_sync_to_core_clock() now makes SystemCoreClock truthful
             * and holds the tick at 1 ms -- so both terms are individually
             * correct and the ratio is correct for the honest reason.
             * The invariant survives the fix: DO NOT move this window to a
             * different clock (RTC / STIMER) from the one dwt_delta_us()
             * divides by. If the two ever disagree again, HP duty skews,
             * inferenceFrac clamps to busy, and everything gets billed at
             * inference power. They have to change together. */
            const TickType_t windowEndTicks = xTaskGetTickCount();
            const float32_t publishWindowSec =
                (float32_t)(uint32_t)(windowEndTicks - publishWindowStart) / (float32_t)configTICK_RATE_HZ;
            publishWindowStart = windowEndTicks;
            const uint32_t denRuns = g_hkv_counters[HKV_CNT_PIPE_DEN_RUNS];
            const uint32_t segRuns = g_hkv_counters[HKV_CNT_PIPE_SEG_RUNS];
            const uint32_t metRuns = g_hkv_counters[HKV_CNT_PIPE_MET_RUNS];
            const float32_t infFracInstant =
                stage_duty_frac(denRuns - prevDenRuns, ecgMetResults.denoiseIps, publishWindowSec) +
                stage_duty_frac(segRuns - prevSegRuns, ecgMetResults.segmentIps, publishWindowSec) +
                stage_duty_frac(metRuns - prevMetRuns, ecgMetResults.arrhythmiaIps, publishWindowSec);
            prevDenRuns = denRuns;
            prevSegRuns = segRuns;
            prevMetRuns = metRuns;

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

            /* Three-state split. See the header above CpuProcessTask and the
             * sourced constants in constants.h. */
            float32_t busyFrac = appMetResults.cpuPercUtil / 100.0f;
            if (busyFrac < 0.0f) {
                busyFrac = 0.0f;
            } else if (busyFrac > 1.0f) {
                busyFrac = 1.0f;
            }
            float32_t inferenceFrac = infFracRollingSum / (float32_t)cpuUtilRollingCount;
            /* Clamped to busy, not asserted equal to it: the two come from
             * independent measurements (FreeRTOS run-time stats vs DWT +
             * counters) and nothing guarantees they agree. Over-clamping bills
             * the excess at inference power, which is the conservative
             * direction. */
            if (inferenceFrac < 0.0f) {
                inferenceFrac = 0.0f;
            } else if (inferenceFrac > busyFrac) {
                inferenceFrac = busyFrac;
            }
            float32_t computeFrac = busyFrac - inferenceFrac;
            if (computeFrac < 0.0f) {
                computeFrac = 0.0f;
            }
            const float32_t idleFrac = 1.0f - busyFrac;

            /* Operating point, read once per window. appState.speedMode is a
             * live dashboard control (TIO_UIO_SPEED_MODE_IDX -> set_speed_mode
             * -> nsx_power_set_performance_mode), so the busy-state figures
             * have to follow it or HP mode reports MORE battery life than LP
             * while drawing ~3x the power. Sleep power is shared. See the
             * constant pairs and their sources in constants.h. */
            const bool hpMode = (appState.speedMode != 0);
            const float32_t inferencePowerMw = inference_power_mw(hpMode);
            const float32_t computePowerMw =
                hpMode ? (float32_t)MCU_COMPUTE_POWER_MW_HP : (float32_t)MCU_COMPUTE_POWER_MW_LP;

            const float32_t avgPower = (inferenceFrac * inferencePowerMw + computeFrac * computePowerMw +
                                        idleFrac * (float32_t)MCU_SLEEP_POWER_MW) /
                                       (float32_t)SYSTEM_POWER_MARGIN;

            appMetResults.battInferenceFrac = inferenceFrac;
            appMetResults.battAvgPowerMw = avgPower;
            /* avgPower is bounded below by idle power for any real fraction set,
             * so the guard is defensive against a constant being zeroed rather
             * than a reachable state. */
            appMetResults.batteryDays = (avgPower > 0.0f) ? ((float32_t)BATT_POWER_CAP / avgPower / 24.0f) : 0.0f;

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

/* USB delivery, owned by TioProcessTask. The decision logic (hold, retry,
 * stall, and whether to take another packet off the queue) lives in
 * src/tio_tx_sm.h so it can be driven by tests/test_tio_tx_sm.c on the host;
 * everything below is the wiring to FreeRTOS, nsx and the counter table.
 *
 * USB BUSY invariant: tio_usb_send_slot_packet() refuses a frame the TinyUSB
 * FIFO cannot take whole (NSX_USB_STATUS_BUSY) and reports a short write as
 * NSX_USB_STATUS_PARTIAL. Neither counts as delivered, and both clear on
 * their own once the host reads. Every other status is terminal for that
 * packet -- TIMEOUT above all, since nsx_usb_vendor_send() can spin in it for
 * seconds and must never be re-entered on a retry.
 *
 * A packet leaves g_tioTxQueue for USB only when USB can be offered it now, so
 * a slow host backs the queue up instead of costing packets; the queue, not a
 * one-packet holding slot, is what absorbs host jitter. Once the backlog
 * reaches TIO_TX_USB_HOLD_WATERMARK the head is drained anyway so BLE keeps
 * receiving, and counted as a USB drop. Retrying the held
 * packet is only cheap because tio_usb_send_slot_packet() checks
 * nsx_usb_vendor_write_available() before entering nsx_usb_vendor_send()
 * (tio_usb.c). If that guard is ever relaxed -- including by the transport
 * extraction in issue #5 -- a retry can block this task in the multi-second
 * USB timeout path and the pacing here has to be rethought. See #56.
 *
 * A retried PARTIAL re-sends the whole 256 B frame; the host resyncs by
 * scanning for the start/stop bytes. */
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

///////////////////////////////////////////////////////////////////////////////
// Report task (periodic subsystem report, EN_APP_REPORT)
///////////////////////////////////////////////////////////////////////////////
//
// See src/obs.h for the line format, the counter/gauge model, and the locking
// argument. This file supplies only the rotation list and the per-subsystem
// `extra` callbacks that append values which do not live in the counter/gauge
// tables (instantaneous ring lengths, servo state, metric results).
//
// WHY A ROTATION AND NOT ONE BURST. Emitting every subsystem back-to-back once
// a second puts the whole per-second SWO cost -- on the order of 20 ms across
// ten lines -- into a single burst inside ReportTask, which runs at the SAME
// priority as the ECG and PPG pump tasks. A 20 ms burst fits inside the 250 ms
// jitter budget, so it would not break anything, but it perturbs exactly the
// cadence the report exists to measure. Emitting ONE subsystem per slot
// spreads the same total over the second and keeps each individual stall to a
// couple of milliseconds.
//
// Each subsystem is still visited exactly once per rotation and the rotation
// is one second, so every `_ps` delta is a genuine per-second rate. The slot
// period is derived from the list length, so adding a subsystem keeps the
// rotation at one second rather than silently stretching the delta window.

/* Every hkv_log_* call in these callbacks runs with the log lock already held
 * (hkv_report_subsystem took it). They must not call hkv_log_begin or
 * hkv_log_end: the mutex is not recursive and a nested begin deadlocks. */

static void
report_extra_sensor(void)
{
    /* sensor.c owns these counters: they are updated from the AS7058 INT ISR
     * and the interval pair carries a ticks->ms conversion, so they stay
     * behind sensor.h's accessors rather than moving into the obs table. The
     * wire format is `k=v` either way, so nothing downstream can tell.
     *
     * Confirms the INT ISR is firing and that both PPG channels and ECG are
     * actually reaching their ringbuffers. A stable isr_min/isr_max pair close
     * to the FIFO watermark's expected interval (~125-130 ms on hardware for
     * this profile) confirms a normal cadence -- the bursty look of
     * watermark-batched delivery is not itself a bug. A max that is a large
     * multiple of the min is real IRQ starvation, and is the first thing to
     * check after any change touching interrupt priorities, the USB/BLE ISR
     * paths, or critical sections. */
    hkv_log_u32("isr", sensor_get_as7058_int_isr_count());
    hkv_log_u32("missed", sensor_get_irq_notify_missed_count());
    hkv_log_u32("ppg_push", sensor_get_ppg_push_count());
    hkv_log_u32("ppg_drop", sensor_get_ppg_drop_count());
    hkv_log_u32("ecg_push", sensor_get_ecg_push_count());
    hkv_log_u32("ecg_drop", sensor_get_ecg_drop_count());
    hkv_log_u32("isr_int_lo_ms", sensor_get_as7058_isr_min_interval_ms());
    hkv_log_u32("isr_int_hi_ms", sensor_get_as7058_isr_max_interval_ms());
    sensor_reset_as7058_isr_interval_stats();
}

static void
report_extra_tio(void)
{
    hkv_log_u32("qdepth", (uint32_t)uxQueueMessagesWaiting(g_tioTxQueue));
}

/* Servo state that is control output rather than an observability counter, so
 * it is read straight from the group instead of being mirrored into a table.
 *
 *   bgt  -- servo budget, extra samples per SERVO WINDOW (64 ticks = 6.4 s),
 *           NOT per second. The rate it encodes is bgt / (window x pump
 *           interval), which should equal the producer's drift. Do NOT expect
 *           a settled value: measured spread is 0..31 clustering near
 *           multiples of samplesPerPkt. See tio_tx_group_servo().
 *   trgh -- the trough the servo last measured, against a target of
 *           TIO_*_TX_TROUGH_TARGET. The shipped servo does not converge;
 *           measured spread is 3..34 against a target of 20. Judge trim_ps and
 *           pump_ps, not this.
 *           CAUTION: trgh 0 with bgt 0 is ambiguous. It is the normal reading
 *           for a producer slower than the pump (self-throttling, not a fault)
 *           AND the reading for a producer that has stopped entirely.
 *           Distinguish them with pump_ps and the `sensor` line, not here. */
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
/* Written by ReportTask immediately before it emits the `cpu` line, read by
 * report_extra_cpu() while the log mutex is held. Single writer, single
 * reader, same task -- the split exists purely to keep the ~1 ms stack walk
 * outside the mutex, not for cross-task safety. */
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
     *   batt_cmp  = util - batt_inf     (compute % of wall time)
     *   batt_idle = 100 - util          (idle % of wall time)
     * `batt_inf` equal to `util` means the clamp is active, i.e. derived
     * inference duty exceeded measured busy. batt_inf is a percentage of wall
     * time; batt_pwr is the modelled average in mW at the LIVE speed mode
     * (see `speed_mode` on the `app` line -- the two must be read together). */
    hkv_log_fx2("batt_inf", 100.0f * appMetResults.battInferenceFrac);
    hkv_log_fx2("batt_pwr", appMetResults.battAvgPowerMw);
    hkv_log_fx2("avg_ips", appMetResults.avgAiIps);
    /* Free stack words on the BLE radio dispatcher task, and the tio_ble_init()
     * status that explains a zero. Both are CACHED VALUES -- the ~1 ms stack
     * walk happens in ReportTask before hkv_report_subsystem() takes the log
     * mutex (see g_cpu_ble_hwm), never from inside this callback. Emitting the
     * scan from here would extend the global log-mutex hold by ~1 ms on top of
     * the ~2 ms of ITM this line already costs, and with EN_APP_TRACE on that
     * blocks every equal-priority pump task's trace call for the duration --
     * measuring the pipeline would perturb it, which is the thing obs.h exists
     * to avoid. See ble_bringup_radio_stack_free_words() for what the scan
     * costs and why it is once per second.
     *
     * Read ble_hwm against BLE_BRINGUP_RADIO_STACK_WORDS (4096); the measured
     * steady-state figure is ~3714, i.e. the stack is ~9% used. `ble_wake_ps`
     * on this same line is the other half of the BLE CPU picture.
     *
     * ble_hwm=0 now means "the radio task is gone" -- BleRadioTask clears its
     * own handle before self-deleting on a tio_ble_init() failure -- and
     * ble_init carries the status code that says why. ble_init=0 with a
     * plausible ble_hwm is the healthy case.
     *
     * OMITTED, not zero-filled, on the two boards with no radio -- same gate
     * as the ble_bringup.h include above. A `ble_hwm=0` would read as a stack
     * about to overflow, which is the opposite of "there is no BLE task". */
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
/* Integer division, so a line count that does not divide 1000 silently yields
 * a rotation SHORTER than a second -- 11 subsystems gives 90 ms slots and a
 * 990 ms rotation -- while every `_ps` field stays labelled per-second and
 * reads ~1% high. Fail the build instead: either pick a divisor of 1000 or
 * change the slot derivation and the `_ps` suffix together. */
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
        /* Sample the ~1 ms BLE stack walk here, OUTSIDE hkv_report_subsystem()
         * and therefore outside the global log mutex it holds for the whole
         * line including extra(). Once per rotation (the cpu slot only), so
         * the cost is unchanged -- what changes is that no other task's trace
         * call waits on it. */
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

#if TIO_USB_ENABLED
    NSX_TRY(tio_usb_init(&tioUsbCtx) != NSX_STATUS_SUCCESS, "TileIO USB Init failed.\n");
#endif

    /* BLE bring-up: app-owned EM9305 radio/WSF-pool/dispatcher-task setup
     * (see ble_bringup.c). Board power-up is already covered by
     * nsx_power_configure() above -- this only layers the BLE-specific
     * ns_ble_pre_init() + radio task creation on top, unlike ble_webble's
     * standalone example (which calls am_bsp_low_power_init() itself,
     * since it has no other board bring-up to reuse). Placed after core
     * inits/queue creation, alongside where tio_usb_init() runs, so both
     * transports come up together before the sensor/processing tasks start
     * producing TileIO packets. */
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

    /* Emitted from main(), before the scheduler starts, so it is the FIRST
     * line of any capture and cannot interleave with a report line. A capture
     * that does not begin with this is a capture whose build is unknown, and
     * every conclusion drawn from it is provisional. */
    hkv_log_boot();

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
