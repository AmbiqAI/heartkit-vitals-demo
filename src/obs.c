// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file obs.c
 * @brief Counter/gauge storage and serialized diagnostic output. See obs.h for
 *        the design argument -- why counters are never gated, why gauges are a
 *        separate concept with an explicit reset, why the lock is a mutex, and
 *        what this change does NOT fix.
 */
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "nsx_core.h"

#include "obs.h"

/* uptime_ms in the line format is xTaskGetTickCount() used directly. That is
 * only milliseconds if the tick is 1 kHz; if this ever changes, every uptime
 * in every capture silently becomes a different unit. */
_Static_assert(configTICK_RATE_HZ == 1000, "HKV line format reports ticks as ms; tick rate must be 1 kHz");

///////////////////////////////////////////////////////////////////////////////
// Counters
///////////////////////////////////////////////////////////////////////////////

volatile uint32_t g_hkv_counters[HKV_COUNTER_COUNT] = {0};

/* The reporter's OWN snapshot. Counters are never reset, so this is the only
 * state a delta depends on; a second consumer could keep its own and neither
 * would disturb the other. */
static uint32_t g_hkv_prev[HKV_COUNTER_COUNT] = {0};

typedef struct {
    const char *subsystem;
    const char *key;
    hkv_emit_t emit;
} hkv_counter_meta_t;

#define HKV_COUNTER_META_ROW(id, sub, key, policy) {(sub), (key), (policy)},
static const hkv_counter_meta_t kCounterMeta[HKV_COUNTER_COUNT] = {HKV_COUNTER_TABLE(HKV_COUNTER_META_ROW)};
#undef HKV_COUNTER_META_ROW

/* Runtime-indexed accessors in obs.h do index arithmetic on these enum values.
 * A table reorder must be a build failure, not a counter that quietly
 * attributes PPG drops to ECG in a capture nobody re-checks. */
_Static_assert(HKV_CNT_TIO_ECG_OK == HKV_CNT_TIO_ECG_NODATA + (int)HKV_TIO_WHICH_OK, "TIO slot counter order");
_Static_assert(HKV_CNT_TIO_ECG_FAIL == HKV_CNT_TIO_ECG_NODATA + (int)HKV_TIO_WHICH_FAIL, "TIO slot counter order");
_Static_assert(HKV_CNT_TIO_ECG_PACKFAIL == HKV_CNT_TIO_ECG_NODATA + (int)HKV_TIO_WHICH_PACKFAIL,
               "TIO slot counter order");
_Static_assert(HKV_CNT_TIO_PPG_NODATA == HKV_CNT_TIO_ECG_NODATA + 4, "TIO slot counter stride");
_Static_assert(HKV_CNT_TIO_CPU_NODATA == HKV_CNT_TIO_ECG_NODATA + 8, "TIO slot counter stride");
_Static_assert(HKV_CNT_USB_ECG_DROP == HKV_CNT_USB_ECG_RETRY + (int)HKV_USB_WHICH_DROP, "USB bucket counter order");
_Static_assert(HKV_CNT_USB_PPG_RETRY == HKV_CNT_USB_ECG_RETRY + 2, "USB bucket counter stride");
_Static_assert(HKV_CNT_USB_CPU_RETRY == HKV_CNT_USB_ECG_RETRY + 4, "USB bucket counter stride");
_Static_assert(HKV_CNT_USB_UIO_RETRY == HKV_CNT_USB_ECG_RETRY + 6, "USB bucket counter stride");

///////////////////////////////////////////////////////////////////////////////
// Gauges
///////////////////////////////////////////////////////////////////////////////

/* HKV_GAUGE_LO_INIT lives in obs_fmt.h alongside hkv_gauge_lo_display(), so
 * the sentinel and the substitution that hides it are defined and tested
 * together rather than a header apart. */

typedef struct {
    volatile uint32_t lo;
    volatile uint32_t hi;
    /* Observations in the current window. Reported as `<key>_n` because
     * `<key>_lo=0` is otherwise ambiguous between "observed zero" and "never
     * observed", which for TX occupancy is the difference between a healthy
     * empty ring and a pump task that never ran. */
    volatile uint32_t n;
} hkv_gauge_t;

typedef struct {
    const char *subsystem;
    const char *key;
    hkv_gauge_kind_t kind;
    hkv_gauge_reset_t reset;
} hkv_gauge_meta_t;

#define HKV_GAUGE_META_ROW(id, sub, key, kind, policy) {(sub), (key), (kind), (policy)},
static const hkv_gauge_meta_t kGaugeMeta[HKV_GAUGE_COUNT] = {HKV_GAUGE_TABLE(HKV_GAUGE_META_ROW)};
#undef HKV_GAUGE_META_ROW

#define HKV_GAUGE_INIT_ROW(id, sub, key, kind, policy) {HKV_GAUGE_LO_INIT, 0, 0},
static hkv_gauge_t g_hkv_gauges[HKV_GAUGE_COUNT] = {HKV_GAUGE_TABLE(HKV_GAUGE_INIT_ROW)};
#undef HKV_GAUGE_INIT_ROW

void
hkv_gauge_observe(hkv_gauge_id_t id, uint32_t value)
{
    /* Same relaxed model as the counters, for the same reason: this runs on
     * the pump tick and the PPG tee path. The worst case of a lost update is
     * one interval reporting a slightly narrow range. */
    g_hkv_gauges[id].n++;
    if (value > g_hkv_gauges[id].hi) {
        g_hkv_gauges[id].hi = value;
    }
    if (value < g_hkv_gauges[id].lo) {
        g_hkv_gauges[id].lo = value;
    }
}

void
hkv_gauge_reset(hkv_gauge_id_t id)
{
    g_hkv_gauges[id].lo = HKV_GAUGE_LO_INIT;
    g_hkv_gauges[id].hi = 0;
    g_hkv_gauges[id].n = 0;
}

///////////////////////////////////////////////////////////////////////////////
// Serialized logging
///////////////////////////////////////////////////////////////////////////////

static SemaphoreHandle_t g_logMutex = NULL;
static uint32_t g_logSeq = 0;

/* Whether the CURRENT begin/end section actually holds the mutex.
 *
 * Written only from inside the locked section (set true immediately after a
 * successful take, cleared immediately before the give) or before the
 * scheduler starts, where main() is the only context that runs. Writing it on
 * the way IN to a blocking take would be a real bug: a second task would clear
 * the flag the current holder is relying on and the holder would never give
 * the mutex back. */
static bool g_logLocked = false;

void
hkv_log_init(void)
{
    g_logMutex = xSemaphoreCreateMutex();
    configASSERT(g_logMutex != NULL);
}

static void
hkv_log_lock(void)
{
    /* Logging from an ISR is not a thing that can be made to work: a mutex
     * take is invalid in interrupt context, and the alternative -- emitting
     * unlocked -- is exactly the data race this module exists to remove.
     * Counters are the ISR-safe half of this API; output is not. */
    configASSERT(!xPortIsInsideInterrupt());

    if (g_logMutex == NULL) {
        return; /* before hkv_log_init(): single-threaded, nothing to race */
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        /* main()'s bring-up prints. Taking a mutex here is invalid, and there
         * is nothing to serialize against anyway. */
        return;
    }
    if (xSemaphoreTake(g_logMutex, portMAX_DELAY) == pdTRUE) {
        g_logLocked = true;
    }
}

static void
hkv_log_unlock(void)
{
    if (g_logLocked) {
        g_logLocked = false;
        (void)xSemaphoreGive(g_logMutex);
    }
}

void
hkv_log_begin(const char *subsystem)
{
    hkv_log_lock();
    nsx_printf("HKV|%lu|%lu|%s|", (unsigned long)xTaskGetTickCount(), (unsigned long)g_logSeq++, subsystem);
}

void
hkv_log_u32(const char *key, uint32_t value)
{
    nsx_printf("%s=%lu ", key, (unsigned long)value);
}

void
hkv_log_i32(const char *key, int32_t value)
{
    nsx_printf("%s=%ld ", key, (long)value);
}

void
hkv_log_str(const char *key, const char *value)
{
    nsx_printf("%s=%s ", key, value);
}

void
hkv_log_fx2(const char *key, float value)
{
    nsx_printf("%s_x100=%ld ", key, (long)hkv_fx2_from_float(value));
}

void
hkv_log_end(void)
{
    nsx_printf("\n");
    hkv_log_unlock();
}

void
hkv_log_boot(void)
{
    hkv_log_begin("boot");
    hkv_log_str("app", "heartkit-vitals-demo");
    hkv_log_str("fw", HKV_FW_VERSION);
    hkv_log_str("board", HKV_BOARD_NAME);
    hkv_log_u32("trace", EN_APP_TRACE);
    hkv_log_u32("report", EN_APP_REPORT);
#if defined(AM_PART_APOLLO510B) && TIO_BLE_ENABLED
    hkv_log_u32("ble", 1);
#else
    hkv_log_u32("ble", 0);
#endif
#if TIO_USB_ENABLED
    hkv_log_u32("usb", 1);
#else
    hkv_log_u32("usb", 0);
#endif
    hkv_log_u32("as7058_app_profile", AS7058_APP_PROFILE);
    hkv_log_u32("as7058_board_profile", AS7058_BOARD_PROFILE);
    hkv_log_u32("tick_hz", configTICK_RATE_HZ);
    hkv_log_end();
}

///////////////////////////////////////////////////////////////////////////////
// Table-driven reporting
///////////////////////////////////////////////////////////////////////////////

/* `<key>_ps`: a per-report-interval delta. The reporter visits each subsystem
 * exactly once per rotation and the rotation is one second, so the delta is a
 * per-second rate and the suffix is honest. */
static void
hkv_log_u32_ps(const char *key, uint32_t value)
{
    nsx_printf("%s_ps=%lu ", key, (unsigned long)value);
}

/* `_n` first, deliberately: it is the field that says whether the other two
 * mean anything, and a human scanning the line should hit it before reading a
 * number into a diagnosis. */
static void
hkv_log_gauge(const char *key, hkv_gauge_kind_t kind, uint32_t lo, uint32_t hi, uint32_t n)
{
    nsx_printf("%s_n=%lu ", key, (unsigned long)n);
    if (kind == HKV_GAUGE_RANGE) {
        nsx_printf("%s_lo=%lu ", key, (unsigned long)hkv_gauge_lo_display(lo));
    }
    nsx_printf("%s_hi=%lu ", key, (unsigned long)hi);
}

void
hkv_report_subsystem(const char *subsystem, void (*extra)(void))
{
    uint32_t i;

    hkv_log_begin(subsystem);

    for (i = 0; i < (uint32_t)HKV_COUNTER_COUNT; i++) {
        uint32_t cur;
        uint32_t delta;

        if (strcmp(kCounterMeta[i].subsystem, subsystem) != 0) {
            continue;
        }
        cur = g_hkv_counters[i];
        /* Unsigned subtraction (hkv_counter_delta, obs_fmt.h), so this stays
         * correct across the counter's 32-bit wrap. That correctness is
         * exactly why the reporter keeps its own snapshot instead of zeroing
         * the counter. */
        delta = hkv_counter_delta(cur, g_hkv_prev[i]);
        g_hkv_prev[i] = cur;

        switch (kCounterMeta[i].emit) {
        case HKV_EMIT_DELTA:
            hkv_log_u32_ps(kCounterMeta[i].key, delta);
            break;
        case HKV_EMIT_BOTH:
            hkv_log_u32_ps(kCounterMeta[i].key, delta);
            hkv_log_u32(kCounterMeta[i].key, cur);
            break;
        case HKV_EMIT_TOTAL:
        default:
            hkv_log_u32(kCounterMeta[i].key, cur);
            break;
        }
    }

    for (i = 0; i < (uint32_t)HKV_GAUGE_COUNT; i++) {
        if (strcmp(kGaugeMeta[i].subsystem, subsystem) != 0) {
            continue;
        }
        hkv_log_gauge(kGaugeMeta[i].key, kGaugeMeta[i].kind, g_hkv_gauges[i].lo, g_hkv_gauges[i].hi,
                      g_hkv_gauges[i].n);
        if (kGaugeMeta[i].reset == HKV_GAUGE_WINDOWED) {
            hkv_gauge_reset((hkv_gauge_id_t)i);
        }
    }

    if (extra != NULL) {
        extra();
    }

    hkv_log_end();
}
