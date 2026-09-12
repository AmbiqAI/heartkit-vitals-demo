// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/** @file ble_bringup.h
 * @brief Application-owned TileIO BLE lifecycle.
 * The application owns WSF pools, the radio task, and IRQ registration. */
#ifndef APP_BLE_BRINGUP_H
#define APP_BLE_BRINGUP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialize radio resources and create the dispatcher task.
 * Call once after board power configuration. GATT initialization is asynchronous;
 * inspect ble_bringup_init_status() for a subsequent initialization failure.
 * @return NSX_STATUS_SUCCESS on setup success, nonzero on failure. */
uint32_t ble_bringup_init(void);

/** @brief Return whether a central is connected to the TileIO BLE service. */
bool ble_bringup_connected(void);

/** @brief Forward a framed TileIO USB packet through BLE without blocking.
 * @param packet Buffer containing a TIO_USB_PACKET_LEN-byte frame.
 * @param length Must equal TIO_USB_PACKET_LEN.
 * @return NSX_STATUS_SUCCESS on success, nonzero on failure or skipped delivery. */
uint32_t ble_bringup_send_slot_packet(const uint8_t *packet, uint32_t length);

/** @brief Return the radio task's minimum free stack in StackType_t words.
 * Returns zero if no task exists. Sample only from reporting context, not the
 * dispatcher loop: this scans unused stack. See AmbiqAI/heartkit-vitals-demo#19. */
uint32_t ble_bringup_radio_stack_free_words(void);

/** @brief Return asynchronous radio initialization status.
 * NS_STATUS_SUCCESS means no failure is recorded, including before initialization
 * finishes and on builds without a radio. Failures latch for the boot. */
int32_t ble_bringup_init_status(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_BRINGUP_H */
