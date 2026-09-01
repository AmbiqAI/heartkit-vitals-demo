/**
 * @file ble_bringup.h
 * @brief App-owned TileIO BLE bring-up (apollo510b_evb only).
 *
 * This header (and its ble_bringup.c implementation) is compiled and linked
 * ONLY for the apollo510b_evb board -- the only target in this app's board
 * family with the EM9305 BLE radio (see CMakeLists.txt's
 * `if(NSX_BOARD STREQUAL "apollo510b_evb")` gate, mirroring nsx.yml's
 * `modules[].boards:` scoping of nsx-tileio-ble/nsx-ble/nsx-cordio). The
 * contents are ALSO guarded by `#if defined(AM_PART_APOLLO510B)` so an
 * accidental non-gated build (e.g. a future CMakeLists.txt refactor mistake)
 * degrades to harmless no-op stubs instead of a hard compile failure on
 * apollo510_evb/apollo330mP_evb, which have no BLE hardware at all.
 *
 * Follows the nsx-tileio-ble / nsx-ble app contract (see those modules'
 * READMEs): the module only builds the GATT service and packs/dispatches
 * TileIO frames on top of nsx-ble; the app owns WSF buffer pool sizing,
 * the FreeRTOS radio dispatcher task + wsfOsDispatcher() loop, BLE IRQ
 * glue/NVIC priority, and board/radio power policy. The radio bring-up
 * plumbing here (WSF pool sizing, RadioTask, EM9305 IRQ vector, NVIC
 * priority) is copied from the working reference app
 * neuralspotx/examples/ble_webble/src/main.c (see its
 * `WEBBLE_WSF_BUFFER_POOLS`/`RadioTask`/`webble_ble_interrupts_init`/
 * `AM_BSP_EM9305_RADIO_INT_ISR` and `setup_task`), scaled up for TileIO's
 * larger (240+ byte) characteristic payloads.
 */
#ifndef APP_BLE_BRINGUP_H
#define APP_BLE_BRINGUP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the EM9305 BLE radio + TileIO BLE GATT service.
 *
 * Sets the EM9305 GPIO IRQ's NVIC priority, calls ns_ble_pre_init(), and
 * creates the app-owned BLE radio dispatcher task (which calls
 * tio_ble_init() and then loops wsfOsDispatcher() -- see ble_bringup.c's
 * BleRadioTask). Call once from main(), after nsx_power_configure() (board
 * power-up is already covered by that call; this only adds BLE-specific
 * pre_init + task creation on top -- see main.cc's call site).
 *
 * @return 0 (NSX_STATUS_SUCCESS) on success, nonzero on failure.
 */
uint32_t ble_bringup_init(void);

/**
 * @brief True when a central is currently connected to the TileIO BLE
 * service. Used by TioProcessTask to skip BLE sends cheaply when nobody is
 * listening, mirroring the USB path's tio_usb_tx_available() check -- a
 * disconnected/slow BLE stack must never block or slow USB delivery.
 */
bool ble_bringup_connected(void);

/**
 * @brief Forward one already-framed TileIO USB packet to the BLE transport.
 *
 * TioProcessTask's queue already holds packets pre-framed for USB by
 * pack_and_enqueue_tio_packet()/tio_usb_pack_slot_data() (start/slot/type/
 * dlen/data/crc/stop, see tio_usb.c). BLE wants raw (slot, slot_type, data,
 * length) tuples, not the USB framing/CRC. Rather than changing the shared
 * queue's item format (which would touch the already hardware-validated USB
 * producer/consumer path), this function unpacks the known fixed-offset USB
 * frame layout back into the raw tuple and calls tio_ble_send_slot_data()/
 * tio_ble_send_uio_state(). See ble_bringup.c for the offset constants,
 * cited against tio_usb.c's TIO_USB_*_IDX defines.
 *
 * Non-blocking: tio_ble_send_slot_data()/tio_ble_send_uio_state() ->
 * ns_ble_send_value() returns immediately (NS_STATUS_FAILURE) when nothing
 * is connected or notifications aren't enabled, so this is safe to call
 * unconditionally from TioProcessTask right after the USB send, without
 * risking a stall of USB delivery.
 *
 * @param packet TIO_USB_PACKET_LEN-byte framed packet (as produced by
 *               tio_usb_pack_slot_data()).
 * @param length Must equal TIO_USB_PACKET_LEN.
 * @return 0 (NSX_STATUS_SUCCESS) on success, nonzero on failure/skip.
 */
uint32_t ble_bringup_send_slot_packet(const uint8_t *packet, uint32_t length);

/**
 * @brief Minimum free stack ever observed on the BLE radio dispatcher task,
 *        in WORDS (FreeRTOS StackType_t units), or 0 if the task does not
 *        exist (non-510B build, or bring-up failed before xTaskCreate).
 *
 * CALL THIS AT MOST ONCE PER SECOND, FROM A REPORTING CONTEXT ONLY.
 *
 * uxTaskGetStackHighWaterMark() is not a cheap read of a stored watermark: it
 * is prvTaskCheckFreeStackSpace(), a BYTE-AT-A-TIME walk of the untouched
 * 0xa5 stack fill. BLE_BRINGUP_RADIO_STACK_WORDS is 4096 and the measured
 * high-water is ~3714 words free, so one call walks ~14.9 KB and costs ~1 ms.
 *
 * That is the entire reason this accessor exists. ble_bringup.c used to make
 * this call INSIDE the dispatcher loop, once per wsfOsDispatcher() iteration,
 * storing the result in a variable nothing ever read. Measured on hardware
 * (issue #19), removing that call took BLE-connected CPU from 53.2% to 38.2%
 * -- 15 points -- and raised the dispatcher wake rate from 148/s to 205/s,
 * because the scan was slow enough to throttle the dispatcher below the real
 * event rate. Sampled once per second from ReportTask instead, the same
 * diagnostic costs ~0.06% CPU.
 *
 * Reported as `ble_hwm` on the `cpu` report line.
 */
uint32_t ble_bringup_radio_stack_free_words(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_BRINGUP_H */
