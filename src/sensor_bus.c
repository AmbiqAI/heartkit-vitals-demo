// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file sensor_bus.c
 * @brief Non-blocking AS7058 register reads over the IOM command queue.
 *
 * Lives in the app rather than in nsx-i2c or nsx-as7058: both are vendored by
 * `nsx sync` and hash-locked in nsx.lock, so an edit there would be reverted
 * on the next sync and would invalidate the lock. The AS7058 OSAL takes its
 * transport as a function-pointer config (as7058_osal_configure), which is
 * the supported way to substitute a read path from outside the module.
 * See #65.
 */

#include "sensor_bus.h"

#include "constants.h"

#if HKV_SENSOR_ASYNC

    #include <string.h>

    #include "FreeRTOS.h"
    #include "semphr.h"
    #include "task.h"

    #include "am_mcu_apollo.h"
    #include "as7058_typedefs.h"
    #include "nsx_core.h"
    #include "nsx_i2c_register_driver.h"

    #if I2C_IOM != 1
        #error "sensor_bus.c defines the IOM1 ISR; give it the matching handler if I2C_IOM moves."
    #endif

/* Command queue entries are written by the CPU and fetched by the command
 * queue engine. Keeping the buffer in TCM (default .bss) puts it outside the
 * data cache, so the two views cannot diverge. */
static uint32_t s_cq_buf[HKV_SENSOR_BUS_CQ_WORDS];

/* IOM DMA is not coherent with the data cache, so the destination is staged
 * here and invalidated on completion before the copy out. Whole cache lines
 * only: a partial line would let the invalidate drop a neighbour's dirty
 * bytes. The tail padding absorbs the DMA writing up to a full word past a
 * non-word-multiple length. */
AM_SHARED_RW __attribute__((aligned(32))) static uint8_t s_rx_buf[AS7058_FIFO_DATA_BUFFER_SIZE + 32u];

_Static_assert(HKV_SENSOR_BUS_MAX_READ_BYTES == AS7058_FIFO_DATA_BUFFER_SIZE,
               "HKV_SENSOR_BUS_TIMEOUT_MS is derived from the FIFO size; keep the two in step");

static nsx_as7058_i2c_transport_t *s_transport = NULL;
static void *s_iom_handle = NULL;
static SemaphoreHandle_t s_done_sem = NULL;
static volatile uint32_t s_xfer_status = 0;
static bool s_ready = false;

/* Stamped into the callback context of each queued transfer and bumped when the
 * caller stops waiting, so a completion can be matched to the read that asked
 * for it. Written only by the sensor task, read by the ISR. See #65. */
static volatile uint32_t s_xfer_gen = 0;

/* Bounded spin used to confirm the IOM has stopped writing s_rx_buf, in 10 us
 * units of the transfer timeout. */
    #define SENSOR_BUS_IDLE_SPINS ((uint32_t)HKV_SENSOR_BUS_TIMEOUT_MS)

static volatile uint32_t s_error_count = 0;
static volatile uint32_t s_fallback_count = 0;
static volatile uint32_t s_reset_count = 0;

/* Set when a timed-out transfer is still outstanding after the idle wait. No
 * transfer, queued or blocking, may be issued on the IOM until it is rebuilt.
 * Written only by the sensor task. See #67. */
static volatile bool s_wedged = false;

/* Half-rebuilt: the disable took but the enable did not, so the instance is
 * down and the next attempt must only re-enable. Disabling again would be
 * applied to an already-disabled instance. See #67. */
static volatile bool s_disabled = false;

static inline uint8_t
sensor_bus_dev_addr(void)
{
    return (0u == s_transport->i2c_addr) ? AS7058_I2C_ADDR : s_transport->i2c_addr;
}

static err_code_t
sensor_bus_read_blocking(uint8_t address, uint16_t number, uint8_t *p_values)
{
    if (NSX_I2C_STATUS_SUCCESS != nsx_i2c_read_sequential_regs(s_transport->p_i2c_cfg, sensor_bus_dev_addr(),
                                                              address, p_values, number)) {
        return ERR_DATA_TRANSFER;
    }
    return ERR_SUCCESS;
}

static void
sensor_bus_complete(void *p_ctx, uint32_t status)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    am_hal_cachectrl_range_t range;

    /* A transfer whose caller already gave up must not report: its status and
     * its buffer belong to nobody, and satisfying the semaphore here would hand
     * the next read a mid-DMA buffer as a success. See #65. */
    if ((uint32_t)(uintptr_t)p_ctx != s_xfer_gen) {
        return;
    }

    s_xfer_status = status;

    /* Invalidate without a clean is correct: after init s_rx_buf is written
     * only by IOM DMA, so no dirty line for it can exist in the cache. */
    range.ui32StartAddr = (uint32_t)s_rx_buf;
    range.ui32Size = (uint32_t)sizeof(s_rx_buf);
    am_hal_cachectrl_dcache_invalidate(&range, false);

    xSemaphoreGiveFromISR(s_done_sem, &higherPriorityTaskWoken);
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

/* Queue empty, module idle and no DMA in flight. DMATIP is the bit the HAL's
 * own CQ pause polls to decide a transfer has stopped moving data
 * (am_hal_iom.c iom_cq_pause), so it is what says s_rx_buf is nobody's. */
static bool
sensor_bus_is_idle(void)
{
    am_hal_iom_status_t iom_status;

    if (AM_HAL_STATUS_SUCCESS != am_hal_iom_status_get(s_iom_handle, &iom_status)) {
        return false;
    }
    return iom_status.bStatIdle && 0u == iom_status.ui32NumPendTransactions &&
           0u == (iom_status.ui32DmaStat & IOM0_DMASTAT_DMATIP_Msk);
}

/* After a timeout the transfer is still queued and may still be writing
 * s_rx_buf, which the next read reuses; hold the caller until the IOM reports
 * no work. See #65. */
static bool
sensor_bus_wait_idle(void)
{
    uint32_t spins;

    for (spins = 0; spins < SENSOR_BUS_IDLE_SPINS; spins++) {
        if (sensor_bus_is_idle()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

/* Rebuild the command queue after a wedged read. Gated on the idle check
 * because the HAL offers no way to take the bus back from a live transfer:
 * am_hal_iom_disable() returns AM_HAL_STATUS_IN_USE while the queue holds
 * pending transactions and does not abort a DMA that is already moving
 * (am_hal_iom.c, and the abort used internally on error is not exported).
 * Once the IOM reports itself quiet, disable/enable is a full rebuild: the
 * enable path re-inits the queue and zeroes the pending counters. Until then
 * the caller keeps failing reads rather than issuing one. See #67. */
static bool
sensor_bus_recover(void)
{
    if (!s_disabled) {
        if (!sensor_bus_is_idle() || AM_HAL_STATUS_SUCCESS != am_hal_iom_disable(s_iom_handle)) {
            return false;
        }
        s_disabled = true;
    }
    if (AM_HAL_STATUS_SUCCESS != am_hal_iom_enable(s_iom_handle)) {
        return false;
    }
    s_disabled = false;

    /* The rebuilt queue cannot deliver the orphaned completion, but the
     * semaphore may still carry it. */
    (void)xSemaphoreTake(s_done_sem, 0);
    s_xfer_gen++;
    s_reset_count++;
    s_wedged = false;
    return true;
}

void
am_iomaster1_isr(void)
{
    uint32_t status;

    if (AM_HAL_STATUS_SUCCESS != am_hal_iom_interrupt_status_get(s_iom_handle, true, &status)) {
        return;
    }

    am_hal_iom_interrupt_clear(s_iom_handle, status);
    if (status) {
        am_hal_iom_interrupt_service(s_iom_handle, status);
    }
}

err_code_t
sensor_bus_init(nsx_as7058_i2c_transport_t *p_transport)
{
    nsx_i2c_config_t *p_cfg;

    if (NULL == p_transport || NULL == p_transport->p_i2c_cfg || NULL == p_transport->p_i2c_cfg->iomHandle) {
        return ERR_POINTER;
    }

    s_transport = p_transport;
    p_cfg = p_transport->p_i2c_cfg;
    s_iom_handle = p_cfg->iomHandle;

    s_done_sem = xSemaphoreCreateBinary();
    if (NULL == s_done_sem) {
        return ERR_SYSTEM_CONFIG;
    }

    p_cfg->sIomCfg.pNBTxnBuf = s_cq_buf;
    p_cfg->sIomCfg.ui32NBTxnBufLength = (uint32_t)(sizeof(s_cq_buf) / sizeof(s_cq_buf[0]));

    if (AM_HAL_STATUS_SUCCESS != am_hal_iom_disable(s_iom_handle)) {
        nsx_printf("sensor_bus: command queue attach failed, staying on the blocking path\n");
        return ERR_SYSTEM_CONFIG;
    }

    if (AM_HAL_STATUS_SUCCESS != am_hal_iom_configure(s_iom_handle, &p_cfg->sIomCfg) ||
        AM_HAL_STATUS_SUCCESS != am_hal_iom_enable(s_iom_handle)) {
        /* The blocking fallback still needs the IOM up, and the disable above
         * took it down; a failed configure would otherwise leave it dead. */
        (void)am_hal_iom_enable(s_iom_handle);
        nsx_printf("sensor_bus: command queue attach failed, staying on the blocking path\n");
        return ERR_SYSTEM_CONFIG;
    }

    /* Below configMAX_SYSCALL_INTERRUPT_PRIORITY so the completion callback
     * may use the FromISR API. */
    NVIC_SetPriority(IOMSTR1_IRQn, AM_IRQ_PRIORITY_DEFAULT);
    NVIC_ClearPendingIRQ(IOMSTR1_IRQn);
    NVIC_EnableIRQ(IOMSTR1_IRQn);

    s_ready = true;
    nsx_printf("sensor_bus: async reads on IOM%d, cq_words=%u rx_bytes=%u\n", (int)p_cfg->iom,
               (unsigned)(sizeof(s_cq_buf) / sizeof(s_cq_buf[0])), (unsigned)sizeof(s_rx_buf));
    return ERR_SUCCESS;
}

err_code_t
sensor_bus_read_registers(void *p_ctx, uint8_t address, uint16_t number, uint8_t *p_values)
{
    am_hal_iom_transfer_t txn;
    err_code_t result;
    uint32_t gen;

    if (NULL == p_ctx || NULL == p_values) {
        return ERR_POINTER;
    }

    /* Ahead of the fallback check: a wedged bus must not take a blocking
     * transfer either, and recovery touches the queue, so it is left to task
     * context. A wedged bus stops the measurement and with it the reads, so the
     * rebuild cannot be left to the next read; sensor_bus_recover_if_wedged()
     * is what drives it from then on. See #67. */
    if (s_wedged) {
        if (pdFALSE != xPortIsInsideInterrupt() || !sensor_bus_recover()) {
            s_error_count++;
            return ERR_DATA_TRANSFER;
        }
    }

    /* Register access before the scheduler starts (probe, profile apply) and
     * anything larger than the staging buffer stays on the blocking path. The
     * bound is the payload size, not sizeof(s_rx_buf): the tail padding is
     * reserved for the DMA write past a non-word-multiple length. */
    if (!s_ready || 0u == number || number > AS7058_FIFO_DATA_BUFFER_SIZE ||
        taskSCHEDULER_RUNNING != xTaskGetSchedulerState() || pdFALSE != xPortIsInsideInterrupt()) {
        s_fallback_count++;
        return sensor_bus_read_blocking(address, number, p_values);
    }

    /* Same transaction shape as nsx_i2c_read_sequential_regs, so a queued read
     * and a blocking read differ only in how completion is observed. */
    memset(&txn, 0, sizeof(txn));
    txn.ui8Priority = 1;
    txn.ui32InstrLen = 1;
    #if defined(AM_PART_APOLLO4L) || defined(AM_PART_APOLLO4P) || defined(AM_PART_APOLLO5A) ||      \
        defined(AM_PART_APOLLO5B) || defined(AM_PART_APOLLO510L) || defined(AM_PART_APOLLO330P)
    txn.ui64Instr = address;
    #else
    txn.ui32Instr = address;
    #endif
    txn.eDirection = AM_HAL_IOM_RX;
    txn.ui32NumBytes = number;
    txn.pui32RxBuffer = (uint32_t *)s_rx_buf;
    txn.bContinue = false;
    txn.uPeerInfo.ui32I2CDevAddr = sensor_bus_dev_addr();

    /* Drops a completion that landed after a previous timeout. */
    (void)xSemaphoreTake(s_done_sem, 0);
    s_xfer_status = AM_HAL_STATUS_SUCCESS;
    gen = ++s_xfer_gen;

    if (AM_HAL_STATUS_SUCCESS !=
        am_hal_iom_nonblocking_transfer(s_iom_handle, &txn, sensor_bus_complete, (void *)(uintptr_t)gen)) {
        s_error_count++;
        s_fallback_count++;
        return sensor_bus_read_blocking(address, number, p_values);
    }

    if (pdTRUE != xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(HKV_SENSOR_BUS_TIMEOUT_MS))) {
        /* Orphan the completion first, then let the DMA finish, so the transfer
         * can neither report nor still be writing s_rx_buf when it is reused. */
        s_xfer_gen++;
        if (!sensor_bus_wait_idle()) {
            s_wedged = true;
            (void)sensor_bus_recover();
        }
        s_error_count++;
        return ERR_DATA_TRANSFER;
    }

    if (AM_HAL_STATUS_SUCCESS != s_xfer_status) {
        s_error_count++;
        result = ERR_DATA_TRANSFER;
    } else {
        memcpy(p_values, s_rx_buf, number);
        result = ERR_SUCCESS;
    }

    return result;
}

bool
sensor_bus_recover_if_wedged(void)
{
    if (!s_wedged) {
        return true;
    }
    if (pdFALSE != xPortIsInsideInterrupt()) {
        return false;
    }
    return sensor_bus_recover();
}

uint32_t
sensor_bus_get_error_count(void)
{
    return s_error_count;
}

uint32_t
sensor_bus_get_fallback_count(void)
{
    return s_fallback_count;
}

uint32_t
sensor_bus_get_reset_count(void)
{
    return s_reset_count;
}

#else // HKV_SENSOR_ASYNC

err_code_t
sensor_bus_init(nsx_as7058_i2c_transport_t *p_transport)
{
    (void)p_transport;
    return ERR_SUCCESS;
}

err_code_t
sensor_bus_read_registers(void *p_ctx, uint8_t address, uint16_t number, uint8_t *p_values)
{
    (void)p_ctx;
    (void)address;
    (void)number;
    (void)p_values;
    return ERR_NOT_SUPPORTED;
}

bool
sensor_bus_recover_if_wedged(void)
{
    return true;
}

uint32_t
sensor_bus_get_error_count(void)
{
    return 0;
}

uint32_t
sensor_bus_get_fallback_count(void)
{
    return 0;
}

uint32_t
sensor_bus_get_reset_count(void)
{
    return 0;
}

#endif // HKV_SENSOR_ASYNC
