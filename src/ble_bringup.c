/**
 * @file ble_bringup.c
 * @brief App-owned TileIO BLE bring-up (apollo510b_evb only).
 *
 * See ble_bringup.h for the full contract/rationale. This whole file is a
 * no-op outside AM_PART_APOLLO510B so an accidental non-gated build (e.g. a
 * CMakeLists.txt mistake) degrades gracefully instead of hard-failing on
 * boards with no BLE hardware.
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

#include "ns_ble.h"
#include "tio_ble.h"
#include "tio_usb.h" /* TIO_USB_PACKET_LEN + frame layout constants (cited below) */

/* ---- WSF buffer pool (app-owned policy) -------------------------------
 * Pattern copied from neuralspotx/examples/ble_webble/src/main.c
 * (WEBBLE_WSF_BUFFER_POOLS/WEBBLE_WSF_BUFFER_SIZE/webbleWsfBuffers), scaled
 * up: TileIO's slot signal/metric characteristics are up to
 * TIO_BLE_SLOT_SIG_BUF_LEN/TIO_BLE_SLOT_MET_BUF_LEN (242) bytes, vs.
 * ble_webble's 1-3 byte characteristics, and there are TIO_BLE_SLOT_COUNT*2+1
 * (9) characteristics that may all have pending ATT buffers concurrently.
 * The largest tier is sized to comfortably hold a 242-byte notification (plus
 * ATT/L2CAP header overhead) with headroom for the 247-byte negotiated MTU,
 * with enough buffer count for all 9 characteristics to be in flight.
 */
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

/* ---- App->BLE callback forwarding --------------------------------------
 * received_slot_data()/received_uio_state() in main.cc already implement the
 * ISR-safety fix for host UIO writes (latch-and-defer via g_uio_pending,
 * applied in TioProcessTask context -- see apply_pending_uio_state()). BLE
 * write callbacks run from the WSF/Cordio ATTS server context (RadioTask,
 * not an ISR, but still not a context that should block or touch
 * non-thread-safe ringbuffers directly), so reuse the exact same latch-and-
 * defer pattern rather than duplicating it: these two wrappers just forward
 * into main.cc's existing (extern "C", non-static) callbacks. */
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

static tio_ble_context_t g_bleBringupCtx = {
    .uio_update_cb = &ble_bringup_uio_update_cb,
    .uio_read_cb = &ble_bringup_uio_read_cb,
    .slot_update_cb = &ble_bringup_slot_update_cb,
    .pool_config = NULL, /* set in ble_bringup_init(): needs &g_bleBringupWsfBuffers */
    .service_name = BLE_BRINGUP_ADV_NAME,
    .base_handle = TIO_BLE_DEFAULT_BASE_HANDLE,
    .notify_period_ms = 200,
    .device_info = &g_bleBringupDeviceInfo,
    .connection_config = &g_bleBringupConnectionConfig,
    .event_handler = &ble_bringup_event_handler,
    .event_context = NULL,
};

/* ---- Radio dispatcher task + IRQ glue (app-owned, per nsx-ble contract) */
#define BLE_BRINGUP_RADIO_STACK_WORDS 4096
#define BLE_BRINGUP_RADIO_TASK_PRIORITY (tskIDLE_PRIORITY + 3)

static TaskHandle_t g_bleBringupRadioTaskHandle;
static volatile uint32_t g_bleBringupRadioStackMinWords = 0;
static volatile int32_t g_bleBringupInitStatus = NS_STATUS_SUCCESS;

/* Apollo510B EM9305 GPIO IRQ fanout. ble_webble's reference implementation
 * defines the raw `AM_BSP_EM9305_RADIO_INT_ISR` vector directly, but that
 * only works there because it doesn't also link nsx-gpio: this app already
 * does (for the AS7058 sensor IRQ), and nsx-gpio pulls in nsx-interrupt,
 * whose Apollo5-family glue (nsx_interrupt_vectors.c) provides a STRONG,
 * non-weak definition of every `am_gpio*_isr` vector (including this radio
 * IRQ's `am_gpio0_607f_isr`) that dispatches to nsx_irq_register()'d
 * handlers. Defining the raw ISR ourselves collides with that (link error:
 * multiple definition of `am_gpio0_607f_isr`) -- register through
 * nsx_irq_register() instead, which also covers the NVIC priority setup
 * that ble_webble does separately via NVIC_SetPriority(). */
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
        vTaskDelete(NULL);
        return;
    }
    nsx_printf("[ble] TileIO BLE service started, advertising as '%s'\n", BLE_BRINGUP_ADV_NAME);
    while (1) {
        g_bleBringupRadioStackMinWords = uxTaskGetStackHighWaterMark(NULL);
        wsfOsDispatcher();
    }
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

    /* Registers the EM9305 GPIO IRQ handler and sets its NVIC priority in
     * one call -- see ble_bringup_radio_irq_handler()'s comment for why this
     * app must go through nsx_irq_register() rather than ble_webble's direct
     * NVIC_SetPriority()+raw-vector-definition approach. */
    if (nsx_irq_register(&irqCfg) != NSX_STATUS_SUCCESS) {
        nsx_printf("[ble] EM9305 IRQ register failed\n");
        return NSX_STATUS_FAILURE;
    }

    /* Legacy compatibility hook; nsx-ble intentionally leaves interrupt
     * vector ownership to the app (see ns_ble_pre_init()'s doc comment in
     * ns_ble.h). */
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
    /* Cheap connected check first: ns_ble_send_value() (inside
     * tio_ble_send_slot_data()/tio_ble_send_uio_state()) already returns
     * immediately on no connection, but skipping the unpack entirely when
     * nobody is listening keeps this call as light as possible from the
     * shared TioProcessTask hot path -- never a source of USB-path
     * slowdown. */
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
        /* BLE's per-characteristic payload cap (240) is smaller than USB's
         * 248-byte frame data field; nothing in this app's current slot
         * payloads (ECG/PPG/CPU signals+metrics) approaches either limit,
         * but guard defensively rather than overflow tio_ble's fixed
         * buffers. */
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

#endif /* AM_PART_APOLLO510B */
