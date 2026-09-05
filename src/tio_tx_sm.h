// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file tio_tx_sm.h
 * @brief TileIO transmit decision logic: hold, retry, stall, dequeue.
 *
 * Dependency-free (stdint/stdbool/string only) so tests/test_tio_tx_sm.c can
 * drive it on the host under ASan/UBSan; FreeRTOS, nsx and the counter table
 * enter only through the ops table below. See #56.
 *
 * INVARIANT: a packet leaves the transport queue for USB only when it can be
 * offered immediately, i.e. no packet is already held. While one is held the
 * queue is the buffer that absorbs host jitter, and USB ordering is preserved
 * by construction. The held packet is retried until it lands or the host goes
 * away; there is no attempt budget, so with a connected host packets are lost
 * only to a full queue at enqueue time, to a terminal send status, and to the
 * hold_watermark release below.
 *
 * The queue is not allowed to become the second transport's latency: once
 * occupancy reaches hold_watermark the head is dequeued and fanned out even
 * though a packet is held. USB cannot have it without reordering behind the
 * held packet, so it counts as a USB drop, and the second transport keeps
 * receiving every packet the queue accepted.
 *
 * Retries are paced by the caller's loop, not by a clock here: while a packet
 * is held the caller may have nothing to block on, so it must delay one poll
 * period itself (see TioProcessTask). That period also paces the release.
 */
#ifndef __HKV_TIO_TX_SM_H
#define __HKV_TIO_TX_SM_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Static-asserted against TIO_USB_PACKET_LEN by the firmware translation
 * unit; kept literal here so the header stays dependency-free. */
#define TIO_TX_SM_PACKET_LEN (256u)

typedef enum {
    TIO_TX_SEND_OK = 0, /* delivered */
    TIO_TX_SEND_BUSY,   /* transport could not take it whole; retry later */
    TIO_TX_SEND_FAIL    /* terminal for this packet */
} tio_tx_send_result_t;

/**
 * @brief Transport and observability callbacks driven by tio_tx_sm_step().
 *
 * @param queue_receive Pop one packet into @p packet; false when none is
 *                      available. May block up to the caller's poll period.
 * @param send          Offer one packet to USB. Must not block.
 * @param now_ms        Monotonic milliseconds, used only for the stall latch.
 * @param queue_depth   Packets currently waiting, excluding any held packet.
 *                      Required for the hold_watermark release; NULL disables
 *                      it and lets a held packet gate the drain indefinitely.
 * @param on_dequeued   Every packet taken off the queue, before the USB
 *                      offer; this is the second-transport fan-out. Optional.
 * @param on_retry      A send returned BUSY. Optional.
 * @param on_drop       A packet was lost at the USB layer. Optional.
 * @param on_stall      A stall episode latched. Optional.
 * @param stall_ms      Hold time after which the stall latch sets.
 * @param hold_watermark Occupancy at which a held packet stops gating the
 *                      drain (TIO_TX_USB_HOLD_WATERMARK).
 */
typedef struct {
    bool (*queue_receive)(void *user, uint8_t *packet);
    tio_tx_send_result_t (*send)(void *user, uint8_t *packet);
    uint32_t (*now_ms)(void *user);
    uint32_t (*queue_depth)(void *user);
    void (*on_dequeued)(void *user, const uint8_t *packet);
    void (*on_retry)(void *user, const uint8_t *packet);
    void (*on_drop)(void *user, const uint8_t *packet);
    void (*on_stall)(void *user);
    uint32_t stall_ms;
    uint32_t hold_watermark;
    void *user;
} tio_tx_ops_t;

typedef struct {
    uint8_t packet[TIO_TX_SM_PACKET_LEN]; /* held packet, valid when pending */
    bool pending;
    uint32_t heldSinceMs; /* now_ms at the first offer of packet[] */
    bool stalled;         /* host mounted but not draining */
} tio_tx_sm_t;

static inline void
tio_tx_sm_reset(tio_tx_sm_t *sm)
{
    memset(sm, 0, sizeof(*sm));
}

static inline void
tio_tx_sm_offer(tio_tx_sm_t *sm, const tio_tx_ops_t *ops, uint8_t *packet)
{
    tio_tx_send_result_t result = ops->send(ops->user, packet);

    if (result == TIO_TX_SEND_OK) {
        sm->pending = false;
        sm->stalled = false;
        return;
    }
    if (result == TIO_TX_SEND_BUSY) {
        if (!sm->pending) {
            memcpy(sm->packet, packet, TIO_TX_SM_PACKET_LEN);
            sm->pending = true;
            sm->heldSinceMs = ops->now_ms(ops->user);
        }
        if (ops->on_retry != NULL) {
            ops->on_retry(ops->user, packet);
        }
        if (!sm->stalled && ((ops->now_ms(ops->user) - sm->heldSinceMs) >= ops->stall_ms)) {
            sm->stalled = true;
            if (ops->on_stall != NULL) {
                ops->on_stall(ops->user);
            }
        }
        return;
    }
    /* Terminal status (a transport timeout above all): the frame can never be
     * re-offered, so this is the one loss path a connected host still has. */
    sm->pending = false;
    if (ops->on_drop != NULL) {
        ops->on_drop(ops->user, packet);
    }
}

/**
 * @brief Give up on a held packet because the host disconnected.
 *
 * The only place a connected host's packet is discarded. Idempotent, so the
 * caller may run it on every iteration the host is absent.
 */
static inline void
tio_tx_sm_host_lost(tio_tx_sm_t *sm, const tio_tx_ops_t *ops)
{
    if (sm->pending) {
        sm->pending = false;
        if (ops->on_drop != NULL) {
            ops->on_drop(ops->user, sm->packet);
        }
    }
    sm->stalled = false;
}

/**
 * @brief Run one drain iteration.
 *
 * @param usb_ready False means the host is gone: a held packet is discarded
 *                  and counted, and the stall latch clears. The queue is
 *                  still drained so a second transport keeps receiving.
 * @return True if a packet was taken off the queue this iteration.
 */
static inline bool
tio_tx_sm_step(tio_tx_sm_t *sm, const tio_tx_ops_t *ops, bool usb_ready)
{
    uint8_t packet[TIO_TX_SM_PACKET_LEN];
    bool release = false;

    if (!usb_ready) {
        tio_tx_sm_host_lost(sm, ops);
    } else if (sm->pending) {
        tio_tx_sm_offer(sm, ops, sm->packet);
        if (sm->pending) {
            /* Still held. Below the watermark the queue absorbs the stall and
             * nothing else moves, which keeps USB in order at no cost. At the
             * watermark the queue has stopped being spare capacity, so the
             * head is released to the fan-out and charged to USB instead. */
            if ((ops->queue_depth == NULL) || (ops->queue_depth(ops->user) < ops->hold_watermark)) {
                return false;
            }
            release = true;
        }
    }

    if (!ops->queue_receive(ops->user, packet)) {
        return false;
    }
    if (ops->on_dequeued != NULL) {
        ops->on_dequeued(ops->user, packet);
    }
    if (release) {
        /* Sending it would reorder it ahead of the held packet. */
        if (ops->on_drop != NULL) {
            ops->on_drop(ops->user, packet);
        }
    } else if (usb_ready) {
        tio_tx_sm_offer(sm, ops, packet);
    }
    return true;
}

#ifdef __cplusplus
}
#endif

#endif // __HKV_TIO_TX_SM_H
