// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file sensor_bus.h
 * @brief Non-blocking AS7058 register reads over the IOM command queue.
 *
 * The nsx-i2c register driver only offers am_hal_iom_blocking_transfer, which
 * polls the IOM FIFO in task context, so the FIFO drain is charged to the
 * sensor task as CPU time. This module owns the same IOM instance nsx-i2c
 * brought up, adds a command-queue buffer to it, and provides an
 * as7058_osal_read_registers_t that queues the transfer and blocks the caller
 * until the IOM ISR reports completion. See #65.
 */

#ifndef HKV_SENSOR_BUS_H
#define HKV_SENSOR_BUS_H

#include <stdbool.h>
#include <stdint.h>

#include "constants.h"
#include "error_codes.h"
#include "nsx_as7058_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attach a command queue to the AS7058's IOM and enable its ISR.
 *
 * Must be called after nsx_i2c_interface_init() has brought the instance up:
 * the IOM is briefly disabled, reconfigured with the command-queue buffer
 * (am_hal_iom_configure rejects a configure while enabled) and re-enabled.
 *
 * @param p_transport Transport the OSAL read callback is registered with;
 *                    supplies the nsx-i2c config and the device address, and
 *                    is retained for the lifetime of the bus.
 * @return ERR_SUCCESS, or ERR_SYSTEM_CONFIG if the IOM would not take the
 *         command queue. On failure the bus stays on the blocking path.
 */
err_code_t sensor_bus_init(nsx_as7058_i2c_transport_t *p_transport);

/**
 * @brief as7058_osal_config_t::read_registers backed by the command queue.
 *
 * Falls back to the nsx-i2c blocking read when the scheduler is not running,
 * when called from an ISR, or when the request does not fit the DMA staging
 * buffer, so it is safe to install for every chiplib register read.
 */
err_code_t sensor_bus_read_registers(void *p_ctx, uint8_t address, uint16_t number, uint8_t *p_values);

/** @brief Reads that returned a transfer error or timed out. */
uint32_t sensor_bus_get_error_count(void);

/** @brief Reads served by the blocking fallback rather than the queue. */
uint32_t sensor_bus_get_fallback_count(void);

#ifdef __cplusplus
}
#endif

#endif // HKV_SENSOR_BUS_H
