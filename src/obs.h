// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file obs.h
 * @brief Counters, windowed gauges, and serialized diagnostic output.
 * Counter updates are ISR-safe; logging is task-only. Each counter requires a
 * single writer. Direct printf calls do not participate in the logging lock.
 * See AmbiqAI/heartkit-vitals-demo#11.
 */
#ifndef __HKV_OBS_H
#define __HKV_OBS_H

#include <stdbool.h>
#include <stdint.h>

#include "constants.h"
#include "obs_fmt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build-provided identity for the boot line. CMakeLists.txt passes the real
 * values; the defaults exist so the file compiles standalone and so an
 * unconfigured build is honestly labelled rather than plausibly mislabelled. */
#ifndef HKV_FW_VERSION
#define HKV_FW_VERSION "dev"
#endif
#ifndef HKV_BOARD_NAME
#define HKV_BOARD_NAME "unknown"
#endif

///////////////////////////////////////////////////////////////////////////////
// Counter table
///////////////////////////////////////////////////////////////////////////////
//
// X(id, subsystem, key, emit policy)
//
// ADDING A COUNTER IS ONE ROW. The enum, the storage, the name/subsystem
// metadata and the reporter's emission all derive from this table, and the
// wire format is `key=value` either way, so nothing downstream needs changing
// either -- not the reporter, not a host-side parser.
//
// ORDERING. The reporter filters by subsystem name, so grouping rows by
// subsystem is only a readability convention and a misplaced row still lands
// on the right line. What is NOT optional: where a runtime index selects the
// counter (TileIO slot, USB bucket) the rows must stay in index order with the
// stride the accessor macro below assumes. That one is enforced by
// static_assert in obs.c rather than left to reviewer attention, because the
// failure mode is silent misattribution in a capture nobody re-checks.

typedef enum {
    HKV_EMIT_TOTAL = 0, /* free-running total only */
    HKV_EMIT_DELTA,     /* per-interval delta only, key gets `_ps` */
    HKV_EMIT_BOTH       /* both; the total keeps the bare key */
} hkv_emit_t;

/* clang-format off */
#define HKV_COUNTER_TABLE(X)                                                                       \
    /* --- tio: packing and enqueue into the transport queue --------------- */                    \
    X(HKV_CNT_TIO_UIO_RX,        "tio",    "uio_rx",       HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_QDROP,         "tio",    "qdrop",        HKV_EMIT_BOTH)                          \
    /* Per TileIO slot, in slot order (0=ECG, 1=PPG, 2=CPU) with stride 4.    */                   \
    /* nodata: pump had no full packet. ok/fail: enqueue result. packfail:    */                   \
    /* tio_usb_pack_slot_data() rejected the call outright.                   */                   \
    X(HKV_CNT_TIO_ECG_NODATA,    "tio",    "ecg_nodata",   HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_ECG_OK,        "tio",    "ecg_ok",       HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_ECG_FAIL,      "tio",    "ecg_fail",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_ECG_PACKFAIL,  "tio",    "ecg_packfail", HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_PPG_NODATA,    "tio",    "ppg_nodata",   HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_PPG_OK,        "tio",    "ppg_ok",       HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_PPG_FAIL,      "tio",    "ppg_fail",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_PPG_PACKFAIL,  "tio",    "ppg_packfail", HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_CPU_NODATA,    "tio",    "cpu_nodata",   HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_CPU_OK,        "tio",    "cpu_ok",       HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_CPU_FAIL,      "tio",    "cpu_fail",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_TIO_CPU_PACKFAIL,  "tio",    "cpu_packfail", HKV_EMIT_TOTAL)                         \
    /* --- tiousb: USB delivery health ------------------------------------- */                    \
    /* Per USB bucket, in bucket order (0=ECG, 1=PPG, 2=CPU, 3=UIO) with      */                   \
    /* stride 2 (retry, drop). retry counts one BUSY send attempt; a held     */                   \
    /* packet is retried until it lands, so retries are not loss.             */                   \
    /* drop is a packet USB gave up on: host disconnect, a terminal send      */                   \
    /* status, or a TIO_TX_USB_HOLD_WATERMARK release (see tio_tx_sm.h);      */                   \
    /* queue-full loss is tio/qdrop. stall counts once per held packet        */                   \
    /* that crosses the jitter budget, not once per stall episode, and        */                   \
    /* clears on the next successful send. See #56.                           */                   \
    X(HKV_CNT_USB_ECG_RETRY,     "tiousb", "ecg_retry",    HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_USB_ECG_DROP,      "tiousb", "ecg_drop",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_USB_PPG_RETRY,     "tiousb", "ppg_retry",    HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_USB_PPG_DROP,      "tiousb", "ppg_drop",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_USB_CPU_RETRY,     "tiousb", "cpu_retry",    HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_USB_CPU_DROP,      "tiousb", "cpu_drop",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_USB_UIO_RETRY,     "tiousb", "uio_retry",    HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_USB_UIO_DROP,      "tiousb", "uio_drop",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_USB_STALL,         "tiousb", "stall",        HKV_EMIT_TOTAL)                         \
    /* --- txecg / txppg: rate-matched emission, per signal slot ------------ */                    \
    /* pump_ps is packets HANDED to the transport (expect 10/s); deliv_ps is  */                   \
    /* those it accepted. pump without deliv means the host is not receiving. */                   \
    /* trim_ps is samples discarded by trim-to-high-water; expect 0 in both   */                   \
    /* clock-drift directions. drain_ps is ticks the servo spent an extra     */                   \
    /* sample on -- a PER-SECOND figure against a per-servo-window budget.    */                   \
    X(HKV_CNT_TXECG_PUMP,        "txecg",  "pump",         HKV_EMIT_DELTA)                         \
    X(HKV_CNT_TXECG_DELIV,       "txecg",  "deliv",        HKV_EMIT_DELTA)                         \
    X(HKV_CNT_TXECG_TRIM,        "txecg",  "trim",         HKV_EMIT_BOTH)                          \
    X(HKV_CNT_TXECG_DRAIN,       "txecg",  "drain",        HKV_EMIT_DELTA)                         \
    X(HKV_CNT_TXPPG_PUMP,        "txppg",  "pump",         HKV_EMIT_DELTA)                         \
    X(HKV_CNT_TXPPG_DELIV,       "txppg",  "deliv",        HKV_EMIT_DELTA)                         \
    X(HKV_CNT_TXPPG_TRIM,        "txppg",  "trim",         HKV_EMIT_BOTH)                          \
    X(HKV_CNT_TXPPG_DRAIN,       "txppg",  "drain",        HKV_EMIT_DELTA)                         \
    /* --- pipe: pipeline progress and per-stage failures ------------------- */                    \
    /* Any sustained climb in an err_* invalidates the waveform regardless of */                   \
    /* how healthy the rates look.                                            */                   \
    X(HKV_CNT_PIPE_PPG_ITERS,    "pipe",   "ppg_iters",    HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_PIPE_PPG_PUSHED,   "pipe",   "ppg_pushed",   HKV_EMIT_BOTH)                          \
    /* Per-stage run counts. These are not just progress indicators: the battery */                \
    /* model (constants.h, CpuProcessTask) derives each stage's inference DUTY   */                \
    /* from its own run RATE over the rolling window x its measured DWT          */                \
    /* duration, rather than assuming the nominal 2 s cadence. A stage that      */                \
    /* stalls or is switched to DSP/off therefore stops being billed at          */                \
    /* inference power on its own, with no separate plumbing.                    */                \
    X(HKV_CNT_PIPE_DEN_RUNS,     "pipe",   "den_runs",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_PIPE_SEG_RUNS,     "pipe",   "seg_runs",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_PIPE_MET_RUNS,     "pipe",   "met_runs",     HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_PIPE_ERR_ECG_DEN,  "pipe",   "err_den",      HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_PIPE_ERR_ECG_SEG,  "pipe",   "err_seg",      HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_PIPE_ERR_ECG_ARR,  "pipe",   "err_arr",      HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_PIPE_ERR_ECG_MET,  "pipe",   "err_met",      HKV_EMIT_TOTAL)                         \
    X(HKV_CNT_PIPE_ERR_PPG_MET,  "pipe",   "err_ppgmet",   HKV_EMIT_TOTAL)                         \
    /* --- cpu: run-time-stats collection health --------------------------- */                    \
    /* uxTaskGetSystemState() returns 0 -- not a truncated list -- when the   */                   \
    /* array is too small. Counting that is the difference between "cpu% is   */                   \
    /* pinned because the array overflowed" and a silent wrong number.        */                   \
    X(HKV_CNT_CPU_STAT_OVERFLOW, "cpu",    "stat_overflow", HKV_EMIT_TOTAL)                        \
    /* --- ble: radio dispatcher wake rate (issue #19) --------------------- */                    \
    /* One increment per BleRadioTask iteration, i.e. per wsfOsDispatcher()   */                   \
    /* return. Reported on the `cpu` line as `ble_wake_ps` rather than on a   */                   \
    /* subsystem of its own: a new row would break the                        */                   \
    /* `1000 % HKV_REPORT_LINE_COUNT == 0` assert below, and wake rate is only */                  \
    /* ever read ALONGSIDE cpu util anyway.                                    */                  \
    /*                                                                         */                  \
    /* HOW TO READ IT. Divide by the packet rate (txecg/txppg `deliv_ps` plus  */                  \
    /* the CPU slot) to get wakes per useful packet. Measured baseline before  */                  \
    /* issue #19: 205 wakes/s for ~24 packets/s = 8.5 wakes per packet,        */                  \
    /* against 0 wakes/s while advertising. Anything well above ~2 is the      */                  \
    /* dispatcher being woken by something that had nothing to send.           */                  \
    /* On the two boards with no EM9305 radio there is no dispatcher task, so  */                  \
    /* this stays 0 forever and `ble_hwm` is absent from the line entirely.    */                  \
    X(HKV_CNT_BLE_WAKE,          "cpu",    "ble_wake",     HKV_EMIT_DELTA)
/* clang-format on */

#define HKV_COUNTER_ENUM_ROW(id, sub, key, policy) id,
typedef enum { HKV_COUNTER_TABLE(HKV_COUNTER_ENUM_ROW) HKV_COUNTER_COUNT } hkv_counter_id_t;
#undef HKV_COUNTER_ENUM_ROW

/* Runtime-indexed accessors. Each is backed by a static_assert on the stride
 * in obs.c, so a table reorder is a build failure rather than a counter that
 * quietly attributes PPG drops to ECG. */
#define HKV_TIO_WHICH_NODATA   (0u)
#define HKV_TIO_WHICH_OK       (1u)
#define HKV_TIO_WHICH_FAIL     (2u)
#define HKV_TIO_WHICH_PACKFAIL (3u)
#define HKV_CNT_TIO_SLOT(slot, which)                                                              \
    ((hkv_counter_id_t)((uint32_t)HKV_CNT_TIO_ECG_NODATA + (uint32_t)(slot) * 4u + (uint32_t)(which)))

#define HKV_USB_WHICH_RETRY (0u)
#define HKV_USB_WHICH_DROP  (1u)
#define HKV_CNT_USB_BUCKET(bucket, which)                                                          \
    ((hkv_counter_id_t)((uint32_t)HKV_CNT_USB_ECG_RETRY + (uint32_t)(bucket) * 2u + (uint32_t)(which)))

/* Storage. Exposed so the increment can be inlined at the call site: this is
 * on the PPG sample path and must stay a load/add/store. Treat it as private
 * to hkv_count()/hkv_count_n(). */
extern volatile uint32_t g_hkv_counters[HKV_COUNTER_COUNT];

/**
 * @brief Add to a counter. Relaxed, lock-free, ISR-safe.
 *
 * Not atomic in the RMW sense: a concurrent update from a higher-priority
 * context can lose one increment. That is deliberate and it is the right
 * trade. These are trend indicators read once a second, the alternative
 * (LDREX/STREX or a critical section) costs an order of magnitude more on the
 * PPG sample path, and a counter that is occasionally one short still answers
 * every question anyone asks of it. Do NOT build control logic on these.
 */
static inline void
hkv_count_n(hkv_counter_id_t id, uint32_t n)
{
    g_hkv_counters[id] += n;
}

static inline void
hkv_count(hkv_counter_id_t id)
{
    g_hkv_counters[id]++;
}

///////////////////////////////////////////////////////////////////////////////
// Gauge table
///////////////////////////////////////////////////////////////////////////////
//
// X(id, subsystem, key, kind, reset policy)
//
// Every gauge emits `<key>_n`, the number of observations in the window. It is
// not decoration: without it `<key>_lo=0` cannot be told apart from a window in
// which the producer never ran at all, and those are opposite diagnoses. `_n=0`
// means the rest of the pair carries no information.

typedef enum {
    HKV_GAUGE_RANGE = 0, /* emit `_lo` and `_hi` */
    HKV_GAUGE_MAX        /* emit `_hi` only; the minimum is not meaningful */
} hkv_gauge_kind_t;

typedef enum {
    HKV_GAUGE_WINDOWED = 0, /* reporter resets after each emit */
    HKV_GAUGE_LIFETIME      /* never reset */
} hkv_gauge_reset_t;

/* clang-format off */
#define HKV_GAUGE_TABLE(X)                                                                         \
    /* TX ring occupancy, sampled pre-trim and pre-pop so it reflects what     */                  \
    /* the producer actually created including whatever the trim discards.     */                  \
    /* RANGE because both ends are read: for a block-structured producer like  */                  \
    /* ECG the low value IS the pre-block residual, i.e. how close the next    */                  \
    /* atomic 200-sample push lands to H, while the high value is the peak the */                  \
    /* trim had to absorb.                                                     */                  \
    X(HKV_GAUGE_TXECG_OCC,  "txecg", "occ",  HKV_GAUGE_RANGE, HKV_GAUGE_WINDOWED)                  \
    X(HKV_GAUGE_TXPPG_OCC,  "txppg", "occ",  HKV_GAUGE_RANGE, HKV_GAUGE_WINDOWED)                  \
    /* Samples teed into the PPG TX rings by one pass of PpgProcessTask, i.e.  */                  \
    /* the OBSERVED structural block.                                          */                  \
    /* MAX, not RANGE: the question this answers is "did a single pass ever    */                  \
    /* exceed TIO_PPG_TX_BLOCK_SAMPLES", and the minimum is structurally 0 --  */                  \
    /* the loop runs on every iteration whether or not samples are waiting, so */                  \
    /* the first empty pass pins a `_lo` at 0 forever and it never carries     */                  \
    /* information again.                                                      */                  \
    /* LIFETIME, not WINDOWED: a single excursion invalidates the derivation   */                  \
    /* of H, and a per-second maximum would scroll that one excursion out of   */                  \
    /* the capture -- the opposite of what this gauge is for.                  */                  \
    X(HKV_GAUGE_PPG_TEE,    "pipe",  "tee",  HKV_GAUGE_MAX,   HKV_GAUGE_LIFETIME)
/* clang-format on */

#define HKV_GAUGE_ENUM_ROW(id, sub, key, kind, policy) id,
typedef enum { HKV_GAUGE_TABLE(HKV_GAUGE_ENUM_ROW) HKV_GAUGE_COUNT } hkv_gauge_id_t;
#undef HKV_GAUGE_ENUM_ROW

/** @brief Record an observation of a gauge's instantaneous value. ISR-safe. */
void hkv_gauge_observe(hkv_gauge_id_t id, uint32_t value);

/** @brief Reset one gauge's window regardless of its declared policy. */
void hkv_gauge_reset(hkv_gauge_id_t id);

///////////////////////////////////////////////////////////////////////////////
// Serialized logging
///////////////////////////////////////////////////////////////////////////////

/**
 * @brief Create the log mutex. Call from main() BEFORE creating any task.
 *
 * Logging before this returns is still safe -- it falls through unlocked,
 * which is correct pre-scheduler -- so an early failure still reports itself.
 */
void hkv_log_init(void);

/**
 * @brief Take the log lock and emit the `HKV|uptime|seq|subsystem|` prefix.
 *
 * Every hkv_log_* call between begin and end runs with the lock HELD, which is
 * what makes format-and-emit atomic against the shared am_util_stdio buffer.
 * Keep the section short: it blocks every other logging task for its duration.
 *
 * Not reentrant. begin/end must pair on one call path with no begin nested
 * inside another; the mutex is non-recursive and a nested begin deadlocks.
 */
void hkv_log_begin(const char *subsystem);

void hkv_log_u32(const char *key, uint32_t value);
void hkv_log_i32(const char *key, int32_t value);
void hkv_log_str(const char *key, const char *value);

/** @brief Emit `<key>_x100=<signed hundredths>`. See obs_fmt.h. */
void hkv_log_fx2(const char *key, float value);

/** @brief Terminate the line and release the log lock. */
void hkv_log_end(void);

/** @brief Emit build identity before periodic diagnostic output. */
void hkv_log_boot(void);

/**
 * @brief Emit one subsystem's report line: table counters, then table gauges,
 *        then whatever @p extra appends.
 *
 * Windowed gauges are reset after they are read. Counter deltas are taken
 * against this reporter's own previous snapshot, so no counter is ever reset.
 *
 * @param subsystem subsystem name; matched against the tables
 * @param extra optional callback to append non-table k=v pairs; called with
 *              the log lock held, so it must only use hkv_log_* and must not
 *              call hkv_log_begin or hkv_log_end
 */
void hkv_report_subsystem(const char *subsystem, void (*extra)(void));

///////////////////////////////////////////////////////////////////////////////
// Tracing (EN_APP_TRACE, default 0)
///////////////////////////////////////////////////////////////////////////////

/**
 * @brief One-off `HKV|...|<sub>|<key>=<value>` trace line.
 *
 * Compiled out entirely when EN_APP_TRACE is 0, including the argument
 * evaluation -- except for a (void) cast that keeps a variable used only by a
 * trace from warning. Enable for a bring-up session; do not ship it on. These
 * sit inside the pump tasks' pipeline branches and each line blocks the caller
 * for the duration of its SWO write.
 */
#if EN_APP_TRACE
#define HKV_TRACE_KV(sub, key, val)                                                                \
    do {                                                                                           \
        hkv_log_begin(sub);                                                                        \
        hkv_log_u32((key), (uint32_t)(val));                                                       \
        hkv_log_end();                                                                             \
    } while (0)
#else
#define HKV_TRACE_KV(sub, key, val)                                                                \
    do {                                                                                           \
        (void)(val);                                                                               \
    } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif // __HKV_OBS_H
