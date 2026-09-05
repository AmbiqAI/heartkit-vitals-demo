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

static nsx_as7058_i2c_transport_t *s_transport = NULL;
static void *s_iom_handle = NULL;
static SemaphoreHandle_t s_done_sem = NULL;
static volatile uint32_t s_xfer_status = 0;
static bool s_ready = false;

static volatile uint32_t s_read_count = 0;
static volatile uint32_t s_byte_count = 0;
static volatile uint32_t s_error_count = 0;
static volatile uint32_t s_fallback_count = 0;

    #if HKV_SENSOR_ASYNC_SPIKE
static volatile uint32_t s_busy_cycles = 0;
        #define SENSOR_BUS_CYCLES_MARK(v) ((v) = DWT->CYCCNT)
        #define SENSOR_BUS_CYCLES_ADD(v) (s_busy_cycles += DWT->CYCCNT - (v))
    #else
        #define SENSOR_BUS_CYCLES_MARK(v) ((void)(v))
        #define SENSOR_BUS_CYCLES_ADD(v) ((void)(v))
    #endif

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
    (void)p_ctx;

    s_xfer_status = status;

    range.ui32StartAddr = (uint32_t)s_rx_buf;
    range.ui32Size = (uint32_t)sizeof(s_rx_buf);
    am_hal_cachectrl_dcache_invalidate(&range, false);

    xSemaphoreGiveFromISR(s_done_sem, &higherPriorityTaskWoken);
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void
am_iomaster1_isr(void)
{
    uint32_t status;

    if (AM_HAL_STATUS_SUCCESS == am_hal_iom_interrupt_status_get(s_iom_handle, true, &status)) {
        if (status) {
            am_hal_iom_interrupt_clear(s_iom_handle, status);
            am_hal_iom_interrupt_service(s_iom_handle, status);
        }
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

    if (am_hal_iom_disable(s_iom_handle) || am_hal_iom_configure(s_iom_handle, &p_cfg->sIomCfg) ||
        am_hal_iom_enable(s_iom_handle)) {
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
    uint32_t startCycles = 0;
    err_code_t result;

    if (NULL == p_ctx || NULL == p_values) {
        return ERR_POINTER;
    }

    /* Register access before the scheduler starts (probe, profile apply) and
     * anything larger than the staging buffer stays on the blocking path. */
    if (!s_ready || 0u == number || number > sizeof(s_rx_buf) ||
        taskSCHEDULER_RUNNING != xTaskGetSchedulerState() || pdFALSE != xPortIsInsideInterrupt()) {
        s_fallback_count++;
        return sensor_bus_read_blocking(address, number, p_values);
    }

    SENSOR_BUS_CYCLES_MARK(startCycles);

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

    (void)xSemaphoreTake(s_done_sem, 0);
    s_xfer_status = AM_HAL_STATUS_SUCCESS;

    if (AM_HAL_STATUS_SUCCESS != am_hal_iom_nonblocking_transfer(s_iom_handle, &txn, sensor_bus_complete, NULL)) {
        s_error_count++;
        s_fallback_count++;
        return sensor_bus_read_blocking(address, number, p_values);
    }

    if (pdTRUE != xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(HKV_SENSOR_BUS_TIMEOUT_MS))) {
        s_error_count++;
        return ERR_DATA_TRANSFER;
    }

    if (AM_HAL_STATUS_SUCCESS != s_xfer_status) {
        s_error_count++;
        result = ERR_DATA_TRANSFER;
    } else {
        memcpy(p_values, s_rx_buf, number);
        s_read_count++;
        s_byte_count += number;
        result = ERR_SUCCESS;
    }

    SENSOR_BUS_CYCLES_ADD(startCycles);
    return result;
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

    #if HKV_SENSOR_ASYNC_SPIKE

uint32_t
sensor_bus_get_read_count(void)
{
    return s_read_count;
}

uint32_t
sensor_bus_get_byte_count(void)
{
    return s_byte_count;
}

uint32_t
sensor_bus_get_busy_cycles(void)
{
    return s_busy_cycles;
}

void
sensor_bus_reset_stats(void)
{
    s_read_count = 0;
    s_byte_count = 0;
    s_error_count = 0;
    s_fallback_count = 0;
    s_busy_cycles = 0;
}

    #endif // HKV_SENSOR_ASYNC_SPIKE

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

#endif // HKV_SENSOR_ASYNC
