// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/** @file ble_bringup.c
 * @brief Application-owned TileIO BLE initialization.
 */
#include "ble_bringup.h"

#if defined(AM_PART_APOLLO510B)

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "am_mcu_apollo.h"
#include "am_bsp.h"

#include "nsx_core.h"
#include "nsx_interrupt.h"

#include "obs.h"

#include "ns_ble.h"
#include "tio_ble.h"
#include "tio_usb.h" /* TIO_USB_PACKET_LEN + frame layout constants (cited below) */

/* WSF pools must accommodate concurrent TileIO payloads and protocol overhead. */
#define BLE_BRINGUP_WSF_BUFFER_POOLS 4
#define BLE_BRINGUP_WSF_BUFFER_SIZE                                            \
    (BLE_BRINGUP_WSF_BUFFER_POOLS * 16 + 16 * 8 + 32 * 8 + 64 * 8 + 280 * 20) / \
        sizeof(uint32_t)

static uint32_t g_bleBringupWsfBufferPool[BLE_BRINGUP_WSF_BUFFER_SIZE];
static wsfBufPoolDesc_t g_bleBringupBufferDescriptors[BLE_BRINGUP_WSF_BUFFER_POOLS] = {
    {16, 8}, {32, 8}, {64, 8}, {280, 20}};
static ns_ble_pool_config_t g_bleBringupWsfBuffers = {
    .pool = g_bleBringupWsfBufferPool,
    .poolSize = sizeof(g_bleBringupWsfBufferPool),
    .desc = g_bleBringupBufferDescriptors,
    .descNum = BLE_BRINGUP_WSF_BUFFER_POOLS,
};

/* Defer BLE writes through the same task-context path as USB controls. */
extern void ble_bringup_slot_update_cb(
    uint8_t slot, uint8_t slot_type, const uint8_t *data, uint32_t length);
extern void ble_bringup_uio_update_cb(const uint8_t *data, uint32_t length);
extern void ble_bringup_uio_read_cb(uint8_t *data, uint32_t length);

/* ---- BLE service objects ----------------------------------------------- */
static ns_ble_pool_config_t *const g_bleBringupPoolConfig = &g_bleBringupWsfBuffers;

#define BLE_BRINGUP_ADV_NAME "HKV-BLE"

static const ns_ble_device_info_t g_bleBringupDeviceInfo = {
    .manufacturerName = "Ambiq",
    .modelNumber = "Apollo510B EVB",
    .serialNumber = "NSX-HKV-BLE-0001",
    .firmwareRevision = "0.1.0",
    .hardwareRevision = "Apollo510B EVB",
    .softwareRevision = "heartkit-vitals-demo",
    .vendorIdSource = NS_BLE_DIS_VENDOR_ID_SOURCE_BLUETOOTH_SIG,
    .vendorId = NS_BLE_COMPANY_ID_AMBIQ,
    .productId = 0x0002,
    .productVersion = 0x0001,
};

static const ns_ble_connection_config_t g_bleBringupConnectionConfig = {
    .preferredMtu = 247,
    .dataLenTxOctets = 251,
    .dataLenTxTime = 0x0848,
    .connIntervalMin = 24,
    .connIntervalMax = 40,
    .connLatency = 0,
    .supervisionTimeout = 600,
};

static void
ble_bringup_event_handler(const ns_ble_event_t *event, void *context)
{
    (void)context;
    switch (event->type) {
    case NS_BLE_EVENT_CONNECTED:
        nsx_printf("[ble] connected conn=%u interval=%u latency=%u timeout=%lu\n", event->connId,
                   event->value0, event->value1, (unsigned long)event->detail);
        break;
    case NS_BLE_EVENT_DISCONNECTED:
        nsx_printf("[ble] disconnected conn=%u reason=0x%02x\n", event->connId, event->value0);
        break;
    case NS_BLE_EVENT_MTU_UPDATED:
        nsx_printf("[ble] negotiated MTU %u\n", event->value0);
        break;
    case NS_BLE_EVENT_HW_ERROR:
        nsx_printf("[ble] hardware error status=0x%02x\n", event->status);
        break;
    default:
        break;
    }
}

/* Async notifications are data-driven. Zero selects the default polling period,
 * not disable; use the maximum period. See AmbiqAI/heartkit-vitals-demo#19. */
#define BLE_BRINGUP_DEAD_POLL_PERIOD_MS 65535u

static tio_ble_context_t g_bleBringupCtx = {
    .uio_update_cb = &ble_bringup_uio_update_cb,
    .uio_read_cb = &ble_bringup_uio_read_cb,
    .slot_update_cb = &ble_bringup_slot_update_cb,
    .pool_config = NULL, /* set in ble_bringup_init(): needs &g_bleBringupWsfBuffers */
    .service_name = BLE_BRINGUP_ADV_NAME,
    .base_handle = TIO_BLE_DEFAULT_BASE_HANDLE,
    .notify_period_ms = BLE_BRINGUP_DEAD_POLL_PERIOD_MS,
    .device_info = &g_bleBringupDeviceInfo,
    .connection_config = &g_bleBringupConnectionConfig,
    .event_handler = &ble_bringup_event_handler,
    .event_context = NULL,
};

/* ---- Radio dispatcher task + IRQ glue (app-owned, per nsx-ble contract) */
#define BLE_BRINGUP_RADIO_STACK_WORDS 4096
#define BLE_BRINGUP_RADIO_TASK_PRIORITY (tskIDLE_PRIORITY + 3)

static TaskHandle_t g_bleBringupRadioTaskHandle;
static volatile int32_t g_bleBringupInitStatus = NS_STATUS_SUCCESS;

/* Register through NSX IRQ dispatch to avoid defining a duplicate GPIO vector. */
static void
ble_bringup_radio_irq_handler(void *ctx)
{
    (void)ctx;
    ns_ble_handle_em9305_gpio_irq(AM_BSP_EM9305_RADIO_INT_IRQ);
}

static void
BleRadioTask(void *pvParameters)
{
    (void)pvParameters;
    uint32_t status = tio_ble_init(&g_bleBringupCtx);
    if (status != NS_STATUS_SUCCESS) {
        nsx_printf("[ble] tio_ble_init failed (status=%ld)\n", (long)status);
        g_bleBringupInitStatus = (int32_t)status;
        /* Clear the shared handle before freeing the task. A concurrent reader can
         * still race deletion; see AmbiqAI/heartkit-vitals-demo#19. */
        g_bleBringupRadioTaskHandle = NULL;
        vTaskDelete(NULL);
        return;
    }
    nsx_printf("[ble] TileIO BLE service started, advertising as '%s'\n", BLE_BRINGUP_ADV_NAME);
    while (1) {
        /* Keep dispatcher instrumentation constant-time; see AmbiqAI/heartkit-vitals-demo#19. */
        hkv_count(HKV_CNT_BLE_WAKE);
        wsfOsDispatcher();
    }
}

uint32_t
ble_bringup_radio_stack_free_words(void)
{
    if (g_bleBringupRadioTaskHandle == NULL) {
        return 0;
    }
    return (uint32_t)uxTaskGetStackHighWaterMark(g_bleBringupRadioTaskHandle);
}

int32_t
ble_bringup_init_status(void)
{
    return g_bleBringupInitStatus;
}

uint32_t
ble_bringup_init(void)
{
    nsx_irq_config_t irqCfg = {
        .api = &nsx_interrupt_current_version,
        .irqn = AM_BSP_EM9305_RADIO_INT_IRQ,
        .handler = ble_bringup_radio_irq_handler,
        .ctx = NULL,
        .priority = 4,
        .enable = true,
    };

    g_bleBringupCtx.pool_config = &g_bleBringupWsfBuffers;
    (void)g_bleBringupPoolConfig;


    if (nsx_irq_register(&irqCfg) != NSX_STATUS_SUCCESS) {
        nsx_printf("[ble] EM9305 IRQ register failed\n");
        return NSX_STATUS_FAILURE;
    }

    /* NSX owns the interrupt vector; this hook intentionally performs no registration. */
    ns_ble_pre_init();

    if (xTaskCreate(BleRadioTask, "BleRadioTask", BLE_BRINGUP_RADIO_STACK_WORDS, NULL,
                     BLE_BRINGUP_RADIO_TASK_PRIORITY, &g_bleBringupRadioTaskHandle) != pdPASS) {
        nsx_printf("[ble] BleRadioTask create failed\n");
        return NSX_STATUS_FAILURE;
    }
    return NSX_STATUS_SUCCESS;
}

bool
ble_bringup_connected(void)
{
    return ns_ble_current_connection_id() != DM_CONN_ID_NONE;
}

/* USB TileIO frame layout (see modules/nsx-tileio/modules/nsx-tileio-usb/
 * src/tio_usb.c's TIO_USB_*_IDX #defines -- not exposed via tio_usb.h, so
 * the fixed offsets are re-cited here rather than duplicating the whole
 * pack/unpack implementation):
 *   [0]      start byte  (0x55)
 *   [1]      slot
 *   [2]      slot_type   (0=signal, 1=metric, 2=uio)
 *   [3..4]   data length, little-endian u16
 *   [5..252] data (248 bytes max)
 *   [253..254] crc16
 *   [255]    stop byte   (0xAA)
 */
#define BLE_BRINGUP_TIO_SLOT_IDX 1u
#define BLE_BRINGUP_TIO_TYPE_IDX 2u
#define BLE_BRINGUP_TIO_DLEN_IDX 3u
#define BLE_BRINGUP_TIO_DATA_IDX 5u
#define BLE_BRINGUP_TIO_SLOT_TYPE_UIO 2u

uint32_t
ble_bringup_send_slot_packet(const uint8_t *packet, uint32_t length)
{
    uint8_t slot;
    uint8_t slot_type;
    uint32_t dlen;
    const uint8_t *data;

    if (packet == NULL || length != TIO_USB_PACKET_LEN) {
        return NSX_STATUS_FAILURE;
    }
    /* Avoid unpacking packets when no BLE subscriber can receive them. */
    if (!ble_bringup_connected()) {
        return NSX_STATUS_FAILURE;
    }

    slot = packet[BLE_BRINGUP_TIO_SLOT_IDX];
    slot_type = packet[BLE_BRINGUP_TIO_TYPE_IDX];
    dlen = (uint32_t)packet[BLE_BRINGUP_TIO_DLEN_IDX] |
           ((uint32_t)packet[BLE_BRINGUP_TIO_DLEN_IDX + 1u] << 8);
    data = &packet[BLE_BRINGUP_TIO_DATA_IDX];

    if (slot_type == BLE_BRINGUP_TIO_SLOT_TYPE_UIO) {
        return tio_ble_send_uio_state(data, dlen);
    }
    if (dlen > TIO_BLE_SLOT_DATA_MAX_LEN) {
        /* Reject payloads that exceed the BLE characteristic capacity. */
        return NSX_STATUS_FAILURE;
    }
    return tio_ble_send_slot_data(slot, slot_type, data, dlen);
}

#else /* !AM_PART_APOLLO510B */

/* Harmless no-op stubs: only reached if this file is ever compiled for a
 * board without the EM9305 radio due to a CMakeLists.txt gating mistake. */
uint32_t
ble_bringup_init(void)
{
    return 0;
}

bool
ble_bringup_connected(void)
{
    return false;
}

uint32_t
ble_bringup_send_slot_packet(const uint8_t *packet, uint32_t length)
{
    (void)packet;
    (void)length;
    return 0;
}

uint32_t
ble_bringup_radio_stack_free_words(void)
{
    return 0;
}

int32_t
ble_bringup_init_status(void)
{
    return 0;
}

#endif /* AM_PART_APOLLO510B */
