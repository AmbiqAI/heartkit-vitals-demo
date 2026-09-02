/**
 * @file obs.h
 * @brief Application observability: always-on counters, windowed gauges, and
 *        serialized machine-parseable diagnostic output.
 *
 * See issue #11 and docs/design/streaming-pipeline.md section 4.
 *
 * ===========================================================================
 * TWO MECHANISMS, NOT ONE
 * ===========================================================================
 *
 * The flags this replaces had it backwards: the CHEAP once-per-second counters
 * were behind EN_APP_DEBUG_LOGS (default 0, off) while the EXPENSIVE
 * per-pipeline-branch prints were behind EN_APP_TIMING_LOGS (default 1, on).
 * The shipped build paid for the costly one and omitted the useful one, and
 * hardware validation needed a hand-edited flag before it could see anything.
 *
 * So there are now two separate things with two different costs:
 *
 *   COUNTERS AND GAUGES -- always compiled, never behind a flag. Recording one
 *     is a relaxed read-modify-write on a volatile uint32_t: load, add, store,
 *     no lock, no barrier, safe from an ISR. At the app's increment rate
 *     (a few hundred per second, dominated by one per PPG sample at 100 Hz)
 *     the total cost is on the order of a thousand cycles per second. There is
 *     no build in which turning these off is worth the loss of evidence.
 *
 *   OUTPUT -- two channels, both flag-gated, because printing is what actually
 *     costs. Every emitted line is ITM/SWO traffic that BLOCKS the calling
 *     task for the duration.
 *       EN_APP_REPORT (constants.h, default 1) -- the periodic subsystem
 *         report driven by ReportTask. One subsystem per rotation slot so the
 *         per-second cost is spread rather than bursted.
 *       EN_APP_TRACE (constants.h, default 0) -- ad hoc per-event lines. Off
 *         by default: these sit inside pipeline branches on the equal-priority
 *         pump tasks whose cadence the whole latency budget depends on.
 *
 * Nothing is lost by leaving EN_APP_TRACE off, which is the point: every value
 * a trace line used to carry is also counted, so a persistently failing stage
 * cannot hide behind a plausible-looking silence.
 *
 * ===========================================================================
 * COUNTERS VS GAUGES (they are not the same and must not be conflated)
 * ===========================================================================
 *
 * A COUNTER is free-running and wrapping. It is NEVER reset -- not by the
 * reporter, not by a host, not on reconnect. uint32_t at the app's rates wraps
 * in decades, and even if it wrapped hourly the unsigned subtraction the
 * reporter uses is still correct across the wrap. The reporter keeps its OWN
 * previous snapshot and emits the delta, so two independent consumers can
 * never steal each other's interval. That property is the whole reason
 * counters are not resettable.
 *
 * A GAUGE is a windowed min/max of an instantaneous quantity (ring occupancy,
 * burst size). A window is meaningless without a reset, so gauges have one --
 * explicitly, and declared per gauge in the table:
 *   HKV_GAUGE_WINDOWED -- reset by the reporter after each emit, so the log
 *     shows a progression rather than a lifetime extreme.
 *   HKV_GAUGE_LIFETIME -- never reset. For a high-water mark whose value is a
 *     claim about a derived constant (see HKV_GAUGE_PPG_TEE), where "the worst
 *     ever seen" is the number that matters and a per-second maximum is not.
 *
 * ===========================================================================
 * LINE FORMAT
 * ===========================================================================
 *
 *     HKV|<uptime_ms>|<seq>|<subsystem>|<k=v> <k=v> ...\n
 *
 * uptime_ms is xTaskGetTickCount(); configTICK_RATE_HZ is 1000 so ticks ARE
 * milliseconds (static_assert in obs.c).
 *
 * seq is a GLOBAL report sequence number incremented under the log lock. It is
 * the load-bearing field. Without it a line that never arrived (SWO overflow,
 * a capture tool dropping bytes) is indistinguishable from firmware that
 * stopped emitting, and telling those two apart by eye has cost real debugging
 * time on this project. With it: contiguous seq and a stalled uptime_ms means
 * the firmware stalled; a gap in seq means the transport dropped a line.
 *
 * Values are INTEGERS ONLY. Real quantities are emitted as fixed point with
 * the scale in the key (`hr_x100=7250`), never as a float and never via the
 * `%d.%02d` split, which is expensive and loses the sign (see obs_fmt.h).
 *
 * Keys are self-describing, so a consumer never needs a schema update when a
 * counter is added:
 *   `<key>`     -- a free-running total
 *   `<key>_ps`  -- a per-report-interval delta, i.e. a rate per second
 *   `<key>_lo`  -- gauge minimum over the window
 *   `<key>_hi`  -- gauge maximum over the window
 *   `<key>_n`   -- observations behind that window; `_n=0` means the `_lo` and
 *                  `_hi` beside it are placeholders, NOT measurements
 *   `<key>_x100`-- signed integer hundredths
 *
 * One subsystem per line, each comfortably under 1024 bytes.
 *
 * ===========================================================================
 * WHY A MUTEX AND NOT A BUFFER, AND NOT A CRITICAL SECTION
 * ===========================================================================
 *
 * The observed SWO corruption (`ppg(r[ecg] denoise err=0`) is a DATA RACE, not
 * interleaved-but-intact lines. nsx_printf -> am_util_stdio_vprintf formats
 * into a single file-static g_prfbuf, and am_util_stdio_vsnprintf routes
 * through the SAME buffer. The usual fix -- format into a private buffer, then
 * emit the result atomically -- therefore DOES NOT WORK on this SDK: the
 * "private" formatting step is itself the shared resource. Format and emit
 * must both happen inside one lock.
 *
 * The lock is a FreeRTOS mutex, NOT taskENTER_CRITICAL. A ~200-character line
 * is roughly 2 ms of ITM writes; masking interrupts for that long would blow
 * the AS7058 bounded-INT window (sensor.c), a hazard already root-caused twice
 * in this codebase. A mutex lets the sensor ISR run while a log line drains.
 *
 * CONSEQUENCES, both of which are enforced rather than documented-and-hoped:
 *   - NEVER LOG FROM AN ISR. Taking a mutex in an ISR is invalid. hkv_log
 *     asserts !xPortIsInsideInterrupt(). Counters remain ISR-safe; output does
 *     not.
 *   - PRE-SCHEDULER IS HANDLED. main()'s bring-up prints run before
 *     vTaskStartScheduler, where taking a mutex is invalid. The lock is
 *     skipped in that state and the line is emitted unlocked, which is safe
 *     precisely because nothing else is running yet.
 *
 * RESIDUAL, stated plainly: this lock only covers callers that go through
 * hkv_log_*. Direct nsx_printf calls elsewhere in the app (sensor.c,
 * ble_bringup.c, as7058_profiles.c, the model TUs) and inside vendored modules
 * are NOT serialized against it and CAN still corrupt a line if they land
 * concurrently. This change does not eliminate the class; it removes the
 * app-level steady-state sources and leaves the rest visible.
 *
 * Two categories, and the distinction is the useful part:
 *   - Bring-up and error paths (most of the above). They fire once at startup
 *     or when something has already gone wrong, so a corrupted line in a
 *     steady-state capture is unlikely and, on an error path, is not the
 *     problem you are debugging anyway.
 *   - STEADY-STATE emitters, which are the ones that actually reproduce the
 *     defect. One was found in review and fixed rather than documented:
 *     ecg_physiokit_segmentation_inference() (ecg_segmentation.cc) emitted
 *     1 + numPeaks raw lines from EcgProcessTask once per ~2 s window whenever
 *     segmentation is in DSP mode -- runtime-selectable over UIO, so reachable
 *     by any user without a rebuild. It is now behind EN_MODEL_VERBOSE_LOGS,
 *     matching ecg_arrhythmia.cc. If you add a print on a periodic path,
 *     either route it through hkv_log_* or gate it; a raw nsx_printf on a
 *     2 s cadence is enough to corrupt a capture.
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
    /* stride 2 (retry, drop). retry>0 with drop==0 is a transient FIFO-full  */                   \
    /* window every packet survived; drop>0 is real USB-side loss.            */                   \
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

/**
 * @brief Emit the self-describing boot line.
 *
 * A capture that does not start with this line is a capture whose build is
 * unknown, and every conclusion drawn from it is provisional.
 */
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
