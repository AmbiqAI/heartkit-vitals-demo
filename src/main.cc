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
#include "ringbuffer.h"
#include "sensor.h"
#include "store.h"

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

static TaskStatus_t xTaskDetails[10];

static QueueHandle_t g_tioTxQueue = NULL;
static volatile uint32_t g_tio_tx_queue_drops = 0;
static const TickType_t kTioTxTaskPollTicks = pdMS_TO_TICKS(10);
/* Total USB send attempts allowed for one held packet (first try + retries),
 * one attempt per drain iteration. Covers a transient FIFO-full window
 * (~80 ms) without ever pausing the drain; past that the host counts as
 * stalled. */
static const uint32_t kTioUsbMaxSendAttempts = 8;
/* While the stall latch is set, packets to skip between USB probe sends, so a
 * mounted-but-not-draining host is polled at roughly producer_rate/10 rather
 * than on every packet. */
static const uint32_t kTioUsbStallProbePackets = 10;
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

/* Legacy IPS scale: 2e6/deltaUs (legacy main.cc used 2000000.0/deltaUs with a
 * true-microsecond ticker) -- the host dashboard expects this scale. Guarded
 * against deltaUs==0 (fast DSP paths + coarse cycle->us division). */
static inline float32_t
ips_from_delta_us(uint32_t deltaUs)
{
    return 2.0e6f / (float32_t)MAX(deltaUs, 1u);
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

static volatile uint32_t g_tio_enqueue_ok[3] = {0};
static volatile uint32_t g_tio_enqueue_fail[3] = {0};
static volatile uint32_t g_tio_pack_fail[3] = {0};
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

/* USB send failures that were deferred for a retry, and packets that were not
 * delivered over USB at all (see TioProcessTask). */
static volatile uint32_t g_tio_usb_retry[TIO_USB_BUCKET_COUNT] = {0};
static volatile uint32_t g_tio_usb_drop[TIO_USB_BUCKET_COUNT] = {0};
/* Times the drain task latched the "host mounted but not draining" state. */
static volatile uint32_t g_tio_usb_stalls = 0;

static void
count_tio_usb_event(volatile uint32_t *counters, const uint8_t packet[TIO_USB_PACKET_LEN])
{
    uint8_t slot;
    if (packet[TIO_PACKET_TYPE_IDX] == TIO_PACKET_TYPE_UIO) {
        counters[TIO_USB_BUCKET_UIO]++;
        return;
    }
    slot = packet[TIO_PACKET_SLOT_IDX];
    if (slot < 3) {
        counters[slot]++;
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

/* UIO responses unblock host controls. They are only queued from task
 * context, so put them ahead of waveform backlog and discard one stale
 * packet if the bounded queue is full. */
static bool
enqueue_tio_packet_priority(const uint8_t packet[TIO_USB_PACKET_LEN])
{
    uint8_t dropped_packet[TIO_USB_PACKET_LEN];

    if (g_tioTxQueue == NULL) {
        g_tio_tx_queue_drops++;
        return false;
    }
    if (xQueueSendToFront(g_tioTxQueue, packet, 0) == pdTRUE) {
        return true;
    }
    if (xQueueReceive(g_tioTxQueue, dropped_packet, 0) != pdTRUE ||
        xQueueSendToFront(g_tioTxQueue, packet, 0) != pdTRUE) {
        g_tio_tx_queue_drops++;
        return false;
    }
    g_tio_tx_queue_drops++;
    return true;
}

static bool
pack_and_enqueue_tio_packet(uint8_t slot, uint8_t slot_type, const void *payload, uint32_t payload_len)
{
    uint8_t packet[TIO_USB_PACKET_LEN];
    bool ok;
    if (tio_usb_pack_slot_data(slot, slot_type, (const uint8_t *)payload, payload_len, packet) != 0) {
        if (slot < 3) {
            g_tio_pack_fail[slot]++;
        }
        return false;
    }
    ok = enqueue_tio_packet(packet);
    if (slot < 3) {
        if (ok) {
            g_tio_enqueue_ok[slot]++;
        } else {
            g_tio_enqueue_fail[slot]++;
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
            nsx_printf("[tio] host connected\n");
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
        nsx_printf("[app] input source: %d (%s)\n", (int)sensorCtx.inputSource,
                   source < NUM_INPUT_PTS ? "canned patient playback" : "live sensor");
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
static volatile uint32_t g_uio_rx_count = 0;
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
    g_uio_rx_count++;
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

static volatile uint32_t g_tio_nodata[3] = {0};

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
    volatile uint32_t trimmed;   /* samples discarded by trim-to-high-water */
    /* `pumped` counts ticks on which a full packet was assembled and handed to
     * the transport. `delivered` counts those the transport actually accepted.
     * They are NOT the same number and the difference matters: samples are
     * popped from the rings before the enqueue result is known, so during a
     * host blackout the rings still drain and pumped keeps climbing at 10/s
     * with trim at 0 while nothing reaches the host. pumped == delivered is
     * the healthy case; a gap between them is real loss, corroborated by
     * g_tio_enqueue_ok/g_tio_enqueue_fail and g_tio_usb_drop. */
    volatile uint32_t pumped;
    volatile uint32_t delivered;
    /* Ticks on which the servo spent an extra sample. Compare against
     * extraBudget: if drained tracks the budget the servo is in control, and
     * if it falls short the guard is holding the pump off a near-empty ring. */
    volatile uint32_t drained;
    /* Occupancy watermarks, sampled pre-trim and pre-pop -- the true peak the
     * producer created, including whatever the trim is about to discard.
     * ReportTask resets them each interval so the log shows a progression
     * rather than a lifetime extreme.
     *
     * For a block-structured producer like ECG, occMin IS the pre-block
     * residual R: the sawtooth trough is the last pump tick before the next
     * block lands. That is the quantity currently being inferred rather than
     * measured, so it is the one worth having.
     *
     * Diagnostics, not control inputs. ReportTask's reset is a plain 32-bit
     * store racing the pump's update; both are atomic on Cortex-M, so the
     * worst case is one interval reporting a slightly narrow range. */
    volatile uint32_t occMax;
    volatile uint32_t occMin;
} tio_tx_group_t;

#define TIO_OCC_MIN_INIT (0xFFFFFFFFu)

static tio_tx_group_t g_ecgTxGroup = {{&rbEcgMaskTx, &rbEcgRawTx, &rbEcgDenTx},
                                      3,
                                      TIO_ECG_TX_HIGH_WATER,
                                      TIO_ECG_SAMPLES_PER_PKT,
                                      TIO_ECG_TX_TROUGH_TARGET,
                                      0, 0, TIO_OCC_MIN_INIT, TIO_ECG_TX_TROUGH_TARGET, 0, 0,
                                      0, 0, 0, 0, 0,
                                      TIO_OCC_MIN_INIT};
static tio_tx_group_t g_ppgTxGroup = {{&rbPpg1Tx, &rbPpg2Tx, NULL},
                                      2,
                                      TIO_PPG_TX_HIGH_WATER,
                                      TIO_PPG_SAMPLES_PER_PKT,
                                      TIO_PPG_TX_TROUGH_TARGET,
                                      0, 0, TIO_OCC_MIN_INIT, TIO_PPG_TX_TROUGH_TARGET, 0, 0,
                                      0, 0, 0, 0, 0,
                                      TIO_OCC_MIN_INIT};

/* Peak ring occupancy is not H: the trim runs BEFORE the pop, so the producer
 * can land a full structural block on top of (H - samplesPerPkt) samples that
 * survived the previous tick. Assert on that, not on H alone -- asserting
 * H < BUF_LEN would still pass if a window constant grew enough to overrun.
 *
 * The drift drain does not enter this bound: popping the extra sample only
 * ever leaves FEWER samples behind, so the worst case is still the tick that
 * pops the nominal count. */
static_assert(TIO_ECG_TX_HIGH_WATER - TIO_ECG_SAMPLES_PER_PKT + TIO_ECG_TX_BLOCK_SAMPLES <= ECG_TX_BUF_LEN,
              "ECG TX peak occupancy (H - pkt + block) exceeds ring capacity");
static_assert(TIO_PPG_TX_HIGH_WATER - TIO_PPG_SAMPLES_PER_PKT + TIO_PPG_TX_BLOCK_SAMPLES <= PPG_TX_BUF_LEN,
              "PPG TX peak occupancy (H - pkt + block) exceeds ring capacity");

/* A full structural block landing on the trough must still fit under H, or the
 * trim fires on the very next tick and discards fresh signal.
 *
 * The trough is NOT the target: it settles up to PULL_DIV above it (the
 * position term's deadband) and alternates a further samplesPerPkt with the
 * block-period quantisation. Both are included, because the naive
 * target + block form passed comfortably at a target of 30 while the actual
 * peak sat at 248 against an H of 250. */
static_assert(TIO_ECG_TX_TROUGH_TARGET + TIO_TX_SERVO_PULL_DIV + TIO_ECG_SAMPLES_PER_PKT +
                      TIO_ECG_TX_BLOCK_SAMPLES <=
                  TIO_ECG_TX_HIGH_WATER,
              "ECG worst-case trough + block must fit under H");
static_assert(TIO_PPG_TX_TROUGH_TARGET + TIO_TX_SERVO_PULL_DIV + TIO_PPG_SAMPLES_PER_PKT +
                      TIO_PPG_TX_BLOCK_SAMPLES <=
                  TIO_PPG_TX_HIGH_WATER,
              "PPG worst-case trough + block must fit under H");

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
 *    span at least one producer block. So the servo sees the trough, not the
 *    2 s sawtooth whose 200-sample swing would otherwise swamp the ~2/s drift
 *    it is trying to measure.
 *  - The budget is clamped to [0, TIO_TX_SERVO_MAX_BUDGET], so there is no
 *    windup: a producer that stops entirely parks the budget at 0 rather than
 *    accumulating a debt to spend later as a burst.
 *  - Convergence is monotone from below, so the trough approaches target
 *    without overshooting into the trim.
 *  - It settles from both directions. Producer fast: trough rises, budget
 *    rises, extra draining. Producer slow: trough falls, budget falls to 0 and
 *    the pump self-throttles by skipping, which is the correct response since
 *    samples cannot be manufactured.
 *
 * RESIDUAL BEHAVIOUR, measured and accepted. The trough does not sit exactly
 * on target and the budget does not sit exactly still:
 *
 *  - The trough settles ABOVE target, by up to PULL_DIV, because the position
 *    term's integer division has no restoring force inside its deadband.
 *  - It alternates a further ~samplesPerPkt because the producer block period
 *    is a non-integer number of pump ticks; the window is sized so the
 *    measured minimum is stable despite it, but the underlying occupancy still
 *    alternates.
 *  - The budget therefore dithers by a sample or two window to window.
 *
 * None of this is a defect to chase. Both bounds are accounted for in the peak
 * static_assert above, so the trim cannot fire; the trough stays a full packet
 * clear of empty, so emission stays at 10 pkt/s; and the dither is a fraction
 * of a sample per second of rate error. An earlier 32-tick window turned this
 * same quantisation into a genuine 0<->10 budget slam -- see
 * TIO_TX_SERVO_WINDOW_TICKS for why the window length is the fix and damping
 * the gain would have been treating the symptom. */
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
    if ((uint32_t)avail > group->occMax) {
        group->occMax = (uint32_t)avail;
    }
    if ((uint32_t)avail < group->occMin) {
        group->occMin = (uint32_t)avail;
    }
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
        group->trimmed += (uint32_t)drop;
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
        group->drained++;
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
        g_tio_nodata[0]++;
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
    g_ecgTxGroup.pumped++;
    if (pack_and_enqueue_tio_packet(0, 0, buffer, length)) {
        g_ecgTxGroup.delivered++;
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
        g_tio_nodata[1]++;
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
    g_ppgTxGroup.pumped++;
    if (pack_and_enqueue_tio_packet(1, 0, buffer, length)) {
        g_ppgTxGroup.delivered++;
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
        g_tio_nodata[2]++;
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

static volatile uint32_t g_ecg_seg_runs = 0;

/* Per-stage non-zero-return counts, indexed by tio_stage_err_t.
 *
 * Always compiled, unlike the EN_APP_TIMING_LOGS prints these sit beside: with
 * those prints off (the default) the inference return codes are otherwise
 * discarded at the `(void)err`, so a persistently failing denoise or
 * segmentation stage would produce a plausible-looking flat trace and no
 * indication anywhere that the model never ran. Two increments per 2 s. */
typedef enum {
    kStageErrEcgDenoise = 0,
    kStageErrEcgSegment,
    kStageErrEcgMetrics,
    kStageErrPpgMetrics,
    kStageErrCount
} tio_stage_err_t;

static volatile uint32_t g_stage_err[kStageErrCount] = {0};

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
            ecgMetResults.denoiseuIpspw = 1.0e3f * ecgMetResults.denoiseIps / AVG_INFERENCE_POWER;
            if (err != 0) {
                g_stage_err[kStageErrEcgDenoise]++;
            }
#if EN_APP_TIMING_LOGS
            nsx_printf("[ecg] denoise err=%lu\n", (unsigned long)err);
#endif
        }

        ///////////////////////////////////////////////////////////////////
        // ECG SEGMENTATION
        ///////////////////////////////////////////////////////////////////
        else if (ringbuffer_len(&rbEcgSeg) >= ECG_SEG_WINDOW_LEN) {
            tickStart = dwt_cycles();
            g_ecg_seg_runs++;
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
            ecgMetResults.segmentuIpspw = 1.0e3f * ecgMetResults.segmentIps / AVG_INFERENCE_POWER;
            if (err != 0) {
                g_stage_err[kStageErrEcgSegment]++;
            }
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

            ecgMetResults.arrhythmiaIps = ips_from_delta_us(dwt_delta_us(tickStart));
            ecgMetResults.arrhythmiaIpspw = 1.0e3f * ecgMetResults.arrhythmiaIps / AVG_INFERENCE_POWER;

            send_ecg_metrics();
            if (err != 0) {
                g_stage_err[kStageErrEcgMetrics]++;
            }
#if EN_APP_TIMING_LOGS
            nsx_printf("[ecg] metrics err=%lu\n", (unsigned long)err);
#endif
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

static volatile uint32_t g_ppg_loop_iters = 0;
static volatile uint32_t g_ppg_samples_pushed = 0;

/* Largest number of samples teed into the PPG TX rings by a single pass of the
 * loop below, i.e. the OBSERVED structural block.
 *
 * TIO_PPG_TX_BLOCK_SAMPLES (13) is an empirical figure taken from the AS7058
 * watermark interval, not a compile-time bound: with PPG_DS_RATE == 1 the loop
 * is bounded only by MIN(len(rbPpg1Sensor), len(rbPpg2Sensor)), so a delayed
 * task could in principle tee up to SENSOR_BUF_LEN-1 samples in one pass and
 * exceed H. Unlike ECG, whose block is ECG_SEG_VALID_LEN and therefore
 * statically checkable, this one has to be watched at runtime. If this counter
 * reports above TIO_PPG_TX_BLOCK_SAMPLES on the bench, that constant is wrong
 * and H must be re-derived from the real bound. */
static volatile uint32_t g_ppg_tee_burst_max = 0;

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
        g_ppg_loop_iters++;
        service_ppg_flush_request();
        size_t numSamples = MIN(ringbuffer_len(&rbPpg1Sensor), ringbuffer_len(&rbPpg2Sensor));
        if (numSamples / PPG_DS_RATE > g_ppg_tee_burst_max) {
            g_ppg_tee_burst_max = (uint32_t)(numSamples / PPG_DS_RATE);
        }
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
            g_ppg_samples_pushed++;
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
                g_stage_err[kStageErrPpgMetrics]++;
            }
#if EN_APP_TIMING_LOGS
            if (err != 0) {
                nsx_printf("[ppg] metrics err=%lu\n", (unsigned long)err);
            }
#endif
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        (void)err; /* only consumed by the EN_APP_TIMING_LOGS print above */

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
        service_cpu_flush_request();
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

/* USB delivery state owned by TioProcessTask.
 *
 * USB BUSY invariant: tio_usb_send_slot_packet() refuses a frame the TinyUSB
 * FIFO cannot take whole (NSX_USB_STATUS_BUSY) and reports a short write as
 * NSX_USB_STATUS_PARTIAL. Neither counts as delivered.
 *  - Transient: the packet is held in `packet` and retried once per drain
 *    iteration, up to kTioUsbMaxSendAttempts (~80 ms). USB ordering is
 *    preserved, so a packet drained while one is held cannot be sent and is
 *    dropped deliberately and counted.
 *  - Sustained: when that budget runs out -- host still mounted but not
 *    draining, e.g. the dashboard died without a clean close -- `stalled`
 *    latches. USB sends are then skipped and counted, except one probe send
 *    every kTioUsbStallProbePackets packets; a probe that succeeds clears the
 *    latch and delivers that packet. Probing is only cheap because
 *    tio_usb_send_slot_packet() checks nsx_usb_vendor_write_available()
 *    before entering nsx_usb_vendor_send() (tio_usb.c) and so returns BUSY
 *    immediately against a stalled host. If that guard is ever relaxed --
 *    including by the transport extraction in issue #5 -- each probe can
 *    block this task in the multi-second USB timeout path, and the probe
 *    interval here has to be rethought.
 * The drain itself runs at full rate in every state, so BLE keeps receiving
 * 100% of packets and a stalled host cannot back the queue up into
 * producer-side drops or starve UIO/metric traffic. The task never blocks on
 * USB. A retried PARTIAL re-sends the whole 256 B frame; the host resyncs by
 * scanning for the start/stop bytes. */
typedef struct
{
    uint8_t packet[TIO_USB_PACKET_LEN]; /* packet held for retry */
    bool pending;                       /* packet[] is valid */
    uint32_t attempts;                  /* send attempts spent on packet[] */
    TickType_t lastAttemptTick;         /* tick of the last attempt on packet[] */
    bool stalled;                       /* host mounted but not draining */
    uint32_t probeCountdown;            /* packets to skip before next probe */
} tio_usb_tx_state_t;

/* BUSY/PARTIAL clear on their own once the host reads. Every other status is
 * terminal for this packet -- TIMEOUT above all, since nsx_usb_vendor_send()
 * can spin in it for seconds and must never be re-entered on a retry. */
static bool
tio_usb_status_retryable(uint32_t status)
{
    return (status == NSX_USB_STATUS_BUSY) || (status == NSX_USB_STATUS_PARTIAL);
}

static void
tio_usb_enter_stall(tio_usb_tx_state_t *usb)
{
    if (!usb->stalled) {
        g_tio_usb_stalls++;
    }
    usb->stalled = true;
    usb->probeCountdown = kTioUsbStallProbePackets;
}

/* One retry attempt for a held packet. Never blocks, and never lets the
 * caller skip its queue drain. */
static void
tio_usb_service_pending(tio_usb_tx_state_t *usb, bool usbReady)
{
    uint32_t status;

    if (!usb->pending) {
        return;
    }
    if (!usbReady) {
        /* Host went away mid-retry: the held frame can never land. */
        count_tio_usb_event(g_tio_usb_drop, usb->packet);
        usb->pending = false;
        return;
    }
    /* Space attempts by wall clock, not by drain iterations. With packets
     * already queued xQueueReceive returns immediately, so iteration-paced
     * retries would burn the whole budget inside a millisecond and turn a
     * FIFO-full window that clears in a few ms into a drop plus a spurious
     * stall latch. Returning early here costs no attempt and still falls
     * through to the caller's drain. */
    if ((xTaskGetTickCount() - usb->lastAttemptTick) < kTioTxTaskPollTicks) {
        return;
    }
    usb->lastAttemptTick = xTaskGetTickCount();
    status = tio_usb_send_slot_packet(usb->packet, TIO_USB_PACKET_LEN);
    if (status == NSX_STATUS_SUCCESS) {
        usb->pending = false;
        usb->stalled = false;
        return;
    }
    usb->attempts++;
    if (tio_usb_status_retryable(status) && (usb->attempts < kTioUsbMaxSendAttempts)) {
        count_tio_usb_event(g_tio_usb_retry, usb->packet);
        return;
    }
    count_tio_usb_event(g_tio_usb_drop, usb->packet);
    usb->pending = false;
    tio_usb_enter_stall(usb);
}

/* Offer a freshly drained packet to USB. Caller guarantees usbReady. The
 * buffer is non-const only because tio_usb_send_slot_packet() takes uint8_t*;
 * it is not modified here. */
static void
tio_usb_offer_packet(tio_usb_tx_state_t *usb, uint8_t packet[TIO_USB_PACKET_LEN])
{
    uint32_t status;

    if (usb->pending) {
        /* An older packet is still held: sending this one now would reorder
         * the stream. */
        count_tio_usb_event(g_tio_usb_drop, packet);
        return;
    }
    if (usb->stalled && (usb->probeCountdown > 0)) {
        usb->probeCountdown--;
        count_tio_usb_event(g_tio_usb_drop, packet);
        return;
    }
    status = tio_usb_send_slot_packet(packet, TIO_USB_PACKET_LEN);
    if (status == NSX_STATUS_SUCCESS) {
        /* Includes the probe path: the host is draining again. */
        usb->stalled = false;
        return;
    }
    if (!usb->stalled && tio_usb_status_retryable(status)) {
        memcpy(usb->packet, packet, TIO_USB_PACKET_LEN);
        usb->pending = true;
        usb->attempts = 1;
        usb->lastAttemptTick = xTaskGetTickCount();
        count_tio_usb_event(g_tio_usb_retry, packet);
        return;
    }
    count_tio_usb_event(g_tio_usb_drop, packet);
    tio_usb_enter_stall(usb);
}

void
TioProcessTask(void *pvParameters)
{
    (void)pvParameters;
    uint8_t packet[TIO_USB_PACKET_LEN];
    tio_usb_tx_state_t usb = {};
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
         * Draining once and fanning out to both transports in-line (below)
         * keeps a single-consumer queue with clean semantics, at the cost of
         * a disconnected BLE not being distinguishable from "no work yet" --
         * acceptable since bleReady already covers that case explicitly. */
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
        bool bleReady = ble_bringup_connected();
#else
        bool bleReady = false;
#endif
        /* At most one USB retry attempt per iteration, then fall through: the
         * queue is drained every iteration whatever USB is doing, so BLE
         * fan-out and queue depth are never held hostage to a stalled host
         * (see tio_usb_tx_state_t for the full invariant). */
        tio_usb_service_pending(&usb, usbReady);
        if (!usbReady) {
            /* Nothing to probe once the host is gone; start clean on the next
             * connect. */
            usb.stalled = false;
            usb.probeCountdown = 0;
        }
        if (!usbReady && !bleReady) {
            /* Neither transport has anyone listening: leave packets queued
             * (bounded depth, oldest producer-side drops apply) rather than
             * draining into the void -- matches the pre-BLE USB-only
             * behavior exactly when BLE is compiled out/disconnected. */
            vTaskDelay(kTioTxTaskPollTicks);
            continue;
        }
        if (xQueueReceive(g_tioTxQueue, packet, kTioTxTaskPollTicks) == pdTRUE) {
            if (usbReady) {
                tio_usb_offer_packet(&usb, packet);
            }
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
            if (bleReady) {
                /* Non-blocking (see ble_bringup_send_slot_packet's doc
                 * comment): never allowed to stall USB delivery above. */
                ble_bringup_send_slot_packet(packet, TIO_USB_PACKET_LEN);
            }
#endif
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
        /* Debug sensor breadcrumbs:
         * confirms the AS7058 INT ISR is firing and both PPG channels +
         * ECG are actually flowing into their ringbuffers, useful for
         * verifying sensor bring-up on new hardware/profile changes. */
        nsx_printf("[sensor] isr=%lu missed=%lu ppg(push=%lu drop=%lu) ecg(push=%lu drop=%lu) tio_drops=%lu\n",
                   (unsigned long)sensor_get_as7058_int_isr_count(), (unsigned long)sensor_get_irq_notify_missed_count(),
                   (unsigned long)sensor_get_ppg_push_count(), (unsigned long)sensor_get_ppg_drop_count(),
                   (unsigned long)sensor_get_ecg_push_count(), (unsigned long)sensor_get_ecg_drop_count(),
                   (unsigned long)g_tio_tx_queue_drops);
        /* AS7058 INT ISR-to-ISR interval range over the last report period.
         * A stable, uniform min/max close to the FIFO watermark's expected
         * interval (e.g. ~125-130ms for a ~26-sample/200Hz ECG watermark, as
         * observed on hardware) confirms the INT line is firing at a normal,
         * expected cadence -- the "bursty" look of watermark-batched
         * ringbuffer delivery is not itself a bug. A max interval that's a
         * large multiple of the min would indicate real IRQ starvation/delay
         * and is worth watching for after any change touching interrupt
         * priorities, USB/BLE ISR paths, or critical sections. */
        nsx_printf("[sensor] as7058 isr interval min=%lu ms max=%lu ms\n",
                   (unsigned long)sensor_get_as7058_isr_min_interval_ms(),
                   (unsigned long)sensor_get_as7058_isr_max_interval_ms());
        sensor_reset_as7058_isr_interval_stats();
        /* TEMP diagnostic (tracking down ECG/PPG TileIO starvation): per
         * slot (0=ECG,1=PPG,2=CPU) nodata=numSamples==0 early-return count,
         * ok=successfully enqueued, fail=enqueue attempted but queue was
         * full, packfail=tio_usb_pack_slot_data() itself rejected the call. */
        nsx_printf("[tio] uio_rx=%lu ecg(nodata=%lu ok=%lu fail=%lu packfail=%lu) ppg(nodata=%lu ok=%lu fail=%lu packfail=%lu) "
                   "cpu(nodata=%lu ok=%lu fail=%lu packfail=%lu)\n",
                   (unsigned long)g_uio_rx_count,
                   (unsigned long)g_tio_nodata[0], (unsigned long)g_tio_enqueue_ok[0], (unsigned long)g_tio_enqueue_fail[0],
                   (unsigned long)g_tio_pack_fail[0], (unsigned long)g_tio_nodata[1], (unsigned long)g_tio_enqueue_ok[1],
                   (unsigned long)g_tio_enqueue_fail[1], (unsigned long)g_tio_pack_fail[1], (unsigned long)g_tio_nodata[2],
                   (unsigned long)g_tio_enqueue_ok[2], (unsigned long)g_tio_enqueue_fail[2], (unsigned long)g_tio_pack_fail[2]);
        /* USB delivery health per stream (0=ECG,1=PPG,2=CPU slots, uio=UIO
         * responses): retry=a BUSY/PARTIAL send that was deferred and will be
         * attempted again, drop=packet not delivered over USB at all (retry
         * budget exhausted, skipped while the stall latch was set, or held
         * when the host disconnected). retry>0 with drop==0 is a transient
         * FIFO-full window that every packet survived; drop>0 is real,
         * deliberate USB-side loss (those packets still went out over BLE
         * when BLE is compiled in and connected). */
        nsx_printf("[tio-usb] stall=%lu ecg(retry=%lu drop=%lu) ppg(retry=%lu drop=%lu) cpu(retry=%lu drop=%lu) "
                   "uio(retry=%lu drop=%lu)\n",
                   (unsigned long)g_tio_usb_stalls,
                   (unsigned long)g_tio_usb_retry[0], (unsigned long)g_tio_usb_drop[0],
                   (unsigned long)g_tio_usb_retry[1], (unsigned long)g_tio_usb_drop[1],
                   (unsigned long)g_tio_usb_retry[2], (unsigned long)g_tio_usb_drop[2],
                   (unsigned long)g_tio_usb_retry[TIO_USB_BUCKET_UIO], (unsigned long)g_tio_usb_drop[TIO_USB_BUCKET_UIO]);
        /* TEMP diagnostic: instantaneous ring-buffer occupancy at the exact
         * moment ReportTask samples it -- if these are chronically 0, the
         * producer (EcgProcessTask/PpgProcessTask segmentation/downsample
         * stage) isn't feeding the TX taps; if they're large/climbing, the
         * TX taps are filling but not being drained (queue/consumer side). */
        nsx_printf("[ppg-task] loop_iters=%lu samples_pushed=%lu rbPpg1Sensor_len=%u rbPpg2Sensor_len=%u "
                   "rbPpg1Met_len=%u rbPpg2Met_len=%u\n",
                   (unsigned long)g_ppg_loop_iters, (unsigned long)g_ppg_samples_pushed,
                   (unsigned)ringbuffer_len(&rbPpg1Sensor), (unsigned)ringbuffer_len(&rbPpg2Sensor),
                   (unsigned)ringbuffer_len(&rbPpg1Met), (unsigned)ringbuffer_len(&rbPpg2Met));
        nsx_printf("[tio-len] ecgRawTx=%u ecgDenTx=%u ecgMaskTx=%u ppg1Tx=%u ppg2Tx=%u qdepth=%u\n",
                   (unsigned)ringbuffer_len(&rbEcgRawTx), (unsigned)ringbuffer_len(&rbEcgDenTx),
                   (unsigned)ringbuffer_len(&rbEcgMaskTx), (unsigned)ringbuffer_len(&rbPpg1Tx),
                   (unsigned)ringbuffer_len(&rbPpg2Tx), (unsigned)uxQueueMessagesWaiting(g_tioTxQueue));
        nsx_printf("[ecg-len] rbEcgSensor=%u rbEcgDen=%u rbEcgRawSeg=%u rbEcgSeg=%u rbEcgMet=%u rbEcgMaskMet=%u seg_runs=%lu\n",
                   (unsigned)ringbuffer_len(&rbEcgSensor), (unsigned)ringbuffer_len(&rbEcgDen),
                   (unsigned)ringbuffer_len(&rbEcgRawSeg), (unsigned)ringbuffer_len(&rbEcgSeg),
                   (unsigned)ringbuffer_len(&rbEcgMet), (unsigned)ringbuffer_len(&rbEcgMaskMet),
                   (unsigned long)g_ecg_seg_runs);
        nsx_printf("[ecg] hr=%d.%02d bpm hrv=%d.%02d ms rhythm=%d qos=%d.%02d\n", (int)ecgMetResults.hr,
                   (int)(fabsf(ecgMetResults.hr - (int)ecgMetResults.hr) * 100), (int)ecgMetResults.hrv,
                   (int)(fabsf(ecgMetResults.hrv - (int)ecgMetResults.hrv) * 100), (int)ecgMetResults.arrhythmiaLabel,
                   (int)ecgMetResults.qos, (int)(fabsf(ecgMetResults.qos - (int)ecgMetResults.qos) * 100));
        nsx_printf("[ppg] pr=%d.%02d bpm spo2=%d.%02d qos=%d.%02d\n", (int)ppgMetResults.pr,
                   (int)(fabsf(ppgMetResults.pr - (int)ppgMetResults.pr) * 100), (int)ppgMetResults.spo2,
                   (int)(fabsf(ppgMetResults.spo2 - (int)ppgMetResults.spo2) * 100), (int)ppgMetResults.qos,
                   (int)(fabsf(ppgMetResults.qos - (int)ppgMetResults.qos) * 100));
#endif

#if EN_APP_EMIT_LOGS
        /* Rate-matched emission health, per signal slot (constants.h "TileIO
         * latency budget"). Gated separately from EN_APP_DEBUG_LOGS and on by
         * default: this single 1 Hz line is what the issue #12 acceptance
         * criteria are read from, and pkt_rate below 10/s is the only direct
         * symptom of a pump that has lost its cadence. Defaulting it off
         * alongside EN_APP_TIMING_LOGS would leave a bench run blind.
         *
         * ReportTask runs at 1 Hz, so the deltas ARE the per-second rates.
         *   pkt_rate  -- packets HANDED to the transport; expect 10/s +/- 1.
         *   deliv     -- of those, the ones the transport accepted. pkt_rate
         *                without deliv means the host is not receiving,
         *                however healthy the rest of the line looks.
         *   trim      -- samples discarded by trim-to-high-water. Expect 0 in
         *                both clock-drift directions now that the drift drain
         *                keeps occupancy off H; TIO_TX_TRIM_DRIFT_ALLOWANCE_SPS
         *                is only a backstop for judging a capture. Sustained
         *                non-zero trim is a bug, not a policy working.
         *   drain     -- ticks on which the servo spent an extra sample.
         *                Should track bgt below; falling short of it means the
         *                guard is holding the pump off a near-empty ring.
         *   bgt       -- servo budget, extra samples per servo window. This is
         *                the rate correction it has converged on: bgt /
         *                (window * pump interval) should equal the producer's
         *                drift. A budget pinned at TIO_TX_SERVO_MAX_BUDGET
         *                means drift exceeds what the servo can correct.
         *   trgh      -- trough the servo last measured, against a target of
         *                TIO_*_TX_TROUGH_TARGET. This is the controlled
         *                variable: converged means trgh sits within the
         *                hysteresis band of target and bgt has stopped moving.
         *                trgh at 0 with bgt at 0 means the producer is slower
         *                than the pump, which is self-throttling, not a fault.
         *   occ       -- TX occupancy low..high per interval, sampled pre-trim
         *                and pre-pop. For ECG the low value is the pre-block
         *                residual R, i.e. how close the next atomic 200-sample
         *                push will land to H.
         *   ppg_burst -- high-water mark of one PPG tee pass; must stay <=
         *                TIO_PPG_TX_BLOCK_SAMPLES or H is mis-derived.
         *   stage_err -- cumulative non-zero returns from ECG denoise /
         *                segment / metrics and PPG metrics. Any sustained
         *                climb invalidates the waveform regardless of rate.
         *
         * Split across two lines deliberately: SWO output from concurrent
         * tasks interleaves and corrupts long lines (issue #11), so no single
         * line should have to be trusted on its own. */
        {
            static uint32_t lastEcgPkts = 0, lastPpgPkts = 0, lastEcgTrim = 0, lastPpgTrim = 0;
            static uint32_t lastEcgDeliv = 0, lastPpgDeliv = 0, lastEcgDrain = 0, lastPpgDrain = 0;
            uint32_t ecgPkts = g_ecgTxGroup.pumped, ppgPkts = g_ppgTxGroup.pumped;
            uint32_t ecgTrim = g_ecgTxGroup.trimmed, ppgTrim = g_ppgTxGroup.trimmed;
            uint32_t ecgDeliv = g_ecgTxGroup.delivered, ppgDeliv = g_ppgTxGroup.delivered;
            uint32_t ecgDrain = g_ecgTxGroup.drained, ppgDrain = g_ppgTxGroup.drained;
            uint32_t ecgOccMin = g_ecgTxGroup.occMin, ecgOccMax = g_ecgTxGroup.occMax;
            uint32_t ppgOccMin = g_ppgTxGroup.occMin, ppgOccMax = g_ppgTxGroup.occMax;
            /* Reset the watermarks so the next interval reports afresh. */
            g_ecgTxGroup.occMin = TIO_OCC_MIN_INIT;
            g_ecgTxGroup.occMax = 0;
            g_ppgTxGroup.occMin = TIO_OCC_MIN_INIT;
            g_ppgTxGroup.occMax = 0;
            if (ecgOccMin == TIO_OCC_MIN_INIT) {
                ecgOccMin = 0;
            }
            if (ppgOccMin == TIO_OCC_MIN_INIT) {
                ppgOccMin = 0;
            }
            nsx_printf("[tio-emit] ecg(pkt=%lu/s deliv=%lu/s trim=%lu/s drain=%lu/s occ=%lu..%lu "
                       "bgt=%u trgh=%lu) ppg(pkt=%lu/s deliv=%lu/s trim=%lu/s drain=%lu/s "
                       "occ=%lu..%lu bgt=%u trgh=%lu)\n",
                       (unsigned long)(ecgPkts - lastEcgPkts), (unsigned long)(ecgDeliv - lastEcgDeliv),
                       (unsigned long)(ecgTrim - lastEcgTrim), (unsigned long)(ecgDrain - lastEcgDrain),
                       (unsigned long)ecgOccMin, (unsigned long)ecgOccMax,
                       (unsigned)g_ecgTxGroup.extraBudget, (unsigned long)g_ecgTxGroup.servoTrough,
                       (unsigned long)(ppgPkts - lastPpgPkts), (unsigned long)(ppgDeliv - lastPpgDeliv),
                       (unsigned long)(ppgTrim - lastPpgTrim), (unsigned long)(ppgDrain - lastPpgDrain),
                       (unsigned long)ppgOccMin, (unsigned long)ppgOccMax,
                       (unsigned)g_ppgTxGroup.extraBudget, (unsigned long)g_ppgTxGroup.servoTrough);
            nsx_printf("[tio-health] trim_tot(ecg=%lu ppg=%lu) ppg_burst=%lu "
                       "stage_err(den=%lu seg=%lu met=%lu ppgmet=%lu)\n",
                       (unsigned long)ecgTrim, (unsigned long)ppgTrim, (unsigned long)g_ppg_tee_burst_max,
                       (unsigned long)g_stage_err[kStageErrEcgDenoise], (unsigned long)g_stage_err[kStageErrEcgSegment],
                       (unsigned long)g_stage_err[kStageErrEcgMetrics], (unsigned long)g_stage_err[kStageErrPpgMetrics]);
            lastEcgPkts = ecgPkts;
            lastPpgPkts = ppgPkts;
            lastEcgTrim = ecgTrim;
            lastPpgTrim = ppgTrim;
            lastEcgDeliv = ecgDeliv;
            lastPpgDeliv = ppgDeliv;
            lastEcgDrain = ecgDrain;
            lastPpgDrain = ppgDrain;
        }
#endif

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

    nsx_printf("heartkit-vitals-demo: AS7058 sensing + physiokit/heliaRT DSP+AI pipeline + TileIO USB streaming\n");

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
