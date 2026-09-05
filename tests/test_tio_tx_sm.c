// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file test_tio_tx_sm.c
 * @brief Host tests for the TileIO transmit decision logic (src/tio_tx_sm.h).
 *
 * The defect these cover (#56) was a policy, not a coding error: the drain
 * task took a packet off the queue every 10 ms whether or not USB could take
 * it, held exactly one, and discarded the rest. On the bench that cost 200 to
 * 400 ECG packets per three minutes while the queue sat at depth 0. Nothing
 * about it is visible in a build, and reproducing it needs a host that reads
 * late, so the decision is tested here instead: a fake transport that returns
 * BUSY for a chosen window is the reproduction.
 *
 * The model below is deliberately pessimistic about drain rate -- one packet
 * per 10 ms poll, where the firmware may spin faster while the queue has
 * backlog -- so a passing run bounds the real one.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "test_assert.h"
#include "tio_tx_sm.h"

#define SIM_POLL_MS (10u)
#define SIM_RATE_HZ (21u)  /* ECG 10/s + PPG 10/s + CPU 1/s */
#define SIM_MAX_PACKETS (256u)

/* Bounded FIFO with the firmware's enqueue policy: a full queue rejects the
 * NEW packet and counts it (enqueue_tio_packet, main.cc). */
typedef struct {
    uint8_t slot[TIO_TX_QUEUE_DEPTH][TIO_TX_SM_PACKET_LEN];
    uint32_t head;
    uint32_t count;
    uint32_t drops;
} sim_queue_t;

typedef struct {
    sim_queue_t queue;
    uint32_t now_ms;
    uint32_t busy_from_ms; /* send returns BUSY over [from, to) */
    uint32_t busy_to_ms;
    bool send_fails;       /* terminal status instead of BUSY */
    uint32_t retries;
    uint32_t drops;
    uint32_t stalls;
    uint32_t delivered[SIM_MAX_PACKETS];
    uint32_t delivered_n;
    /* Fan-out is the second transport: what it got, and when. The `when` is
     * what distinguishes "delivered late" from "not delivered during the
     * stall at all", which end-of-run totals cannot. */
    uint32_t fanout_seq[SIM_MAX_PACKETS];
    uint32_t fanout_at_ms[SIM_MAX_PACKETS];
    uint32_t fanout_n;
    uint32_t queue_max;
} sim_t;

static void
sim_fill_packet(uint8_t *packet, uint32_t seq)
{
    memset(packet, (int)(seq & 0xFFu), TIO_TX_SM_PACKET_LEN);
    packet[0] = (uint8_t)(seq & 0xFFu);
    packet[1] = (uint8_t)((seq >> 8) & 0xFFu);
    packet[2] = (uint8_t)((seq >> 16) & 0xFFu);
    packet[3] = (uint8_t)((seq >> 24) & 0xFFu);
}

static uint32_t
sim_packet_seq(const uint8_t *packet)
{
    return (uint32_t)packet[0] | ((uint32_t)packet[1] << 8) | ((uint32_t)packet[2] << 16) |
           ((uint32_t)packet[3] << 24);
}

static void
sim_enqueue(sim_t *sim, uint32_t seq)
{
    uint32_t tail;
    if (sim->queue.count == TIO_TX_QUEUE_DEPTH) {
        sim->queue.drops++;
        return;
    }
    tail = (sim->queue.head + sim->queue.count) % TIO_TX_QUEUE_DEPTH;
    sim_fill_packet(sim->queue.slot[tail], seq);
    sim->queue.count++;
    if (sim->queue.count > sim->queue_max) {
        sim->queue_max = sim->queue.count;
    }
}

static bool
sim_queue_receive(void *user, uint8_t *packet)
{
    sim_t *sim = (sim_t *)user;
    if (sim->queue.count == 0) {
        return false;
    }
    memcpy(packet, sim->queue.slot[sim->queue.head], TIO_TX_SM_PACKET_LEN);
    sim->queue.head = (sim->queue.head + 1) % TIO_TX_QUEUE_DEPTH;
    sim->queue.count--;
    return true;
}

static tio_tx_send_result_t
sim_send(void *user, uint8_t *packet)
{
    sim_t *sim = (sim_t *)user;
    if ((sim->now_ms >= sim->busy_from_ms) && (sim->now_ms < sim->busy_to_ms)) {
        return sim->send_fails ? TIO_TX_SEND_FAIL : TIO_TX_SEND_BUSY;
    }
    if (sim->delivered_n < SIM_MAX_PACKETS) {
        sim->delivered[sim->delivered_n] = sim_packet_seq(packet);
    }
    sim->delivered_n++;
    return TIO_TX_SEND_OK;
}

static uint32_t
sim_now_ms(void *user)
{
    return ((sim_t *)user)->now_ms;
}

static uint32_t
sim_queue_depth(void *user)
{
    return ((sim_t *)user)->queue.count;
}

static void
sim_on_dequeued(void *user, const uint8_t *packet)
{
    sim_t *sim = (sim_t *)user;
    if (sim->fanout_n < SIM_MAX_PACKETS) {
        sim->fanout_seq[sim->fanout_n] = sim_packet_seq(packet);
        sim->fanout_at_ms[sim->fanout_n] = sim->now_ms;
    }
    sim->fanout_n++;
}

static void
sim_on_retry(void *user, const uint8_t *packet)
{
    (void)packet;
    ((sim_t *)user)->retries++;
}

static void
sim_on_drop(void *user, const uint8_t *packet)
{
    (void)packet;
    ((sim_t *)user)->drops++;
}

static void
sim_on_stall(void *user)
{
    ((sim_t *)user)->stalls++;
}

static void
sim_init(sim_t *sim, tio_tx_ops_t *ops)
{
    memset(sim, 0, sizeof(*sim));
    ops->queue_receive = &sim_queue_receive;
    ops->send = &sim_send;
    ops->now_ms = &sim_now_ms;
    ops->queue_depth = &sim_queue_depth;
    ops->on_dequeued = &sim_on_dequeued;
    ops->on_retry = &sim_on_retry;
    ops->on_drop = &sim_on_drop;
    ops->on_stall = &sim_on_stall;
    ops->stall_ms = TIO_JITTER_BUDGET_MS;
    ops->hold_watermark = TIO_TX_USB_HOLD_WATERMARK;
    ops->user = sim;
}

/* Produce at SIM_RATE_HZ for produce_ms, poll the drain every SIM_POLL_MS,
 * and keep polling to run_ms so a post-stall backlog is allowed to land. */
static void
sim_run(sim_t *sim, tio_tx_sm_t *sm, const tio_tx_ops_t *ops, uint32_t produce_ms, uint32_t run_ms)
{
    uint32_t next_seq = 0;
    for (uint32_t t = 0; t < run_ms; t++) {
        sim->now_ms = t;
        while ((t < produce_ms) && ((next_seq * 1000u) / SIM_RATE_HZ <= t)) {
            sim_enqueue(sim, next_seq);
            next_seq++;
        }
        if ((t % SIM_POLL_MS) == 0) {
            tio_tx_sm_step(sm, ops, true);
        }
    }
}

/* Fan-outs whose dequeue instant is before @p t_ms. Sampling this at two
 * points inside a busy window is what proves the second transport kept moving
 * while USB was held, which end-of-run totals cannot distinguish from a burst
 * delivered after the window closed. */
static uint32_t
sim_fanout_before(const sim_t *sim, uint32_t t_ms)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < sim->fanout_n; i++) {
        if (sim->fanout_at_ms[i] < t_ms) {
            n++;
        }
    }
    return n;
}

/* Every produced packet reached the fan-out exactly once and in order: no
 * duplicate, no skip. Valid only when the queue rejected nothing, since a
 * rejected packet never reaches the fan-out at all. */
static void
check_fanout_complete(const sim_t *sim, uint32_t produced)
{
    CHECK_EQ(sim->queue.drops, 0);
    CHECK_EQ(sim->fanout_n, produced);
    for (uint32_t i = 0; i < sim->fanout_n; i++) {
        if (sim->fanout_seq[i] != i) {
            TEST_FAIL("fan-out %u carried seq %u", i, sim->fanout_seq[i]);
        }
    }
}

/* Delivered exactly once, in production order, with no gaps other than the
 * `expect_gaps` packets USB never got. */
static void
check_ordered(const sim_t *sim, uint32_t produced, uint32_t expect_gaps)
{
    uint32_t prev = 0;
    CHECK_EQ(sim->delivered_n + expect_gaps, produced);
    for (uint32_t i = 0; i < sim->delivered_n; i++) {
        if ((i > 0) && (sim->delivered[i] <= prev)) {
            TEST_FAIL("out of order at %u: seq %u after %u", i, sim->delivered[i], prev);
        }
        prev = sim->delivered[i];
    }
}

/* A host read gap inside the jitter budget window costs nothing: the queue
 * holds the stream and every packet lands late but intact. */
static void
test_short_busy_window(void)
{
    sim_t sim;
    tio_tx_sm_t sm;
    tio_tx_ops_t ops;

    TEST_CASE("300 ms busy window");
    sim_init(&sim, &ops);
    tio_tx_sm_reset(&sm);
    sim.busy_from_ms = 750;
    sim.busy_to_ms = 1050;
    sim_run(&sim, &sm, &ops, 3000, 3500);

    CHECK_EQ(sim.drops, 0);
    CHECK_EQ(sim.queue.drops, 0);
    CHECK_EQ(sim.queue.count, 0);
    CHECK(sim.retries > 0);
    /* Peak backlog is the arrivals during the hold, nowhere near the
     * watermark, so nothing is released and USB keeps every packet. */
    CHECK(sim.queue_max < TIO_TX_USB_HOLD_WATERMARK);
    check_fanout_complete(&sim, 63); /* 21/s x 3 s */
    check_ordered(&sim, 63, 0);
    /* 300 ms held is past TIO_JITTER_BUDGET_MS, so the episode is reported
     * even though nothing was lost. That is the point of splitting the
     * counters: a stall is a latency event, not a loss event. */
    CHECK_EQ(sim.stalls, 1);
}

/* Six times the jitter budget, and the last window size that still costs
 * nothing. The hold starts at t=770 with seq 16 (seq 0..15 were delivered
 * before the window opened, and seq 16 arrives at floor(16000/21) = 761 ms).
 * The last arrival before the window closes is seq 47 at
 * floor(47000/21) = 2238 ms, so the backlog peaks at seq 17..47 = 31 packets,
 * one short of TIO_TX_USB_HOLD_WATERMARK. Nothing is released, so no USB
 * packet is lost -- by a single packet of margin. */
static void
test_long_busy_window(void)
{
    sim_t sim;
    tio_tx_sm_t sm;
    tio_tx_ops_t ops;

    TEST_CASE("1500 ms busy window");
    sim_init(&sim, &ops);
    tio_tx_sm_reset(&sm);
    sim.busy_from_ms = 750;
    sim.busy_to_ms = 2250;
    sim_run(&sim, &sm, &ops, 3000, 3500);

    CHECK_EQ(sim.drops, 0);
    CHECK_EQ(sim.queue.drops, 0);
    CHECK_EQ(sim.queue.count, 0);
    CHECK_EQ(sim.queue_max, TIO_TX_USB_HOLD_WATERMARK - 1);
    check_fanout_complete(&sim, 63);
    check_ordered(&sim, 63, 0);
    CHECK_EQ(sim.stalls, 1);
}

/* Long enough that the queue stops being spare capacity. Held from t=770 with
 * seq 16, so the backlog reaches TIO_TX_USB_HOLD_WATERMARK when seq 48 arrives
 * at floor(48000/21) = 2285 ms; from there every arrival up to the last one
 * inside the window (seq 78 at floor(78000/21) = 3714 ms) is matched by one
 * release at the next 10 ms poll, giving 1 + (78 - 48) = 31 releases, i.e.
 * seq 17..47. Those 31 are USB drops and the queue never fills, so unlike the
 * pre-watermark design there are no producer-side rejections. */
static void
test_watermark_release(void)
{
    sim_t sim;
    tio_tx_sm_t sm;
    tio_tx_ops_t ops;

    TEST_CASE("3000 ms busy window crosses the watermark");
    sim_init(&sim, &ops);
    tio_tx_sm_reset(&sm);
    sim.busy_from_ms = 750;
    sim.busy_to_ms = 3750;
    sim_run(&sim, &sm, &ops, 5000, 5500);

    CHECK_EQ(sim.drops, 31);
    CHECK_EQ(sim.queue.drops, 0);
    CHECK_EQ(sim.queue.count, 0);
    CHECK_EQ(sim.queue_max, TIO_TX_USB_HOLD_WATERMARK);
    CHECK_EQ(sim.stalls, 1);
    /* The regression this case exists for: the second transport must keep
     * receiving DURING the window, not in a burst once it closes. Both
     * comparisons straddle the first release at 2290 ms. */
    CHECK(sim_fanout_before(&sim, 3000) > sim_fanout_before(&sim, 2400));
    CHECK(sim_fanout_before(&sim, 3750) > sim_fanout_before(&sim, 3000));
    check_fanout_complete(&sim, 105); /* 21/s x 5 s produced */
    check_ordered(&sim, 105, 31);
}

/* Disconnect is the one path that still discards a held packet. */
static void
test_disconnect_drops_held_packet(void)
{
    sim_t sim;
    tio_tx_sm_t sm;
    tio_tx_ops_t ops;

    TEST_CASE("disconnect while a packet is held");
    sim_init(&sim, &ops);
    tio_tx_sm_reset(&sm);
    sim.busy_from_ms = 0;
    sim.busy_to_ms = 1000;
    sim_enqueue(&sim, 0);
    sim_enqueue(&sim, 1);

    sim.now_ms = 0;
    CHECK(tio_tx_sm_step(&sm, &ops, true));
    CHECK(sm.pending);
    sim.now_ms = 300;
    CHECK(!tio_tx_sm_step(&sm, &ops, true));
    CHECK(sm.stalled);
    CHECK_EQ(sim.stalls, 1);
    CHECK_EQ(sim.delivered_n, 0);
    /* Seq 1 stayed queued while seq 0 was held: no reordering is possible. */
    CHECK_EQ(sim.queue.count, 1);

    /* What TioProcessTask runs on a !usbReady iteration. */
    sim.now_ms = 310;
    tio_tx_sm_host_lost(&sm, &ops);
    CHECK_EQ(sim.drops, 1);
    CHECK(!sm.pending);
    CHECK(!sm.stalled);
    CHECK_EQ(sim.queue.count, 1);

    /* Reset state, host back, backlog resumes from seq 1 with no reordering. */
    sim.now_ms = 1000;
    CHECK(tio_tx_sm_step(&sm, &ops, true));
    CHECK_EQ(sim.delivered_n, 1);
    CHECK_EQ(sim.delivered[0], 1);
    CHECK_EQ(sim.drops, 1);
}

/* With USB gone but a second transport connected the queue is still drained,
 * and nothing is offered to USB. */
static void
test_drain_without_usb(void)
{
    sim_t sim;
    tio_tx_sm_t sm;
    tio_tx_ops_t ops;

    TEST_CASE("drain with no USB host");
    sim_init(&sim, &ops);
    tio_tx_sm_reset(&sm);
    sim_enqueue(&sim, 0);

    CHECK(tio_tx_sm_step(&sm, &ops, false));
    CHECK_EQ(sim.fanout_n, 1);
    CHECK_EQ(sim.delivered_n, 0);
    CHECK_EQ(sim.drops, 0);
    CHECK(!sm.pending);
}

/* A terminal send status cannot be retried, so it is the residual USB-layer
 * loss path with a connected host. */
static void
test_terminal_status_drops(void)
{
    sim_t sim;
    tio_tx_sm_t sm;
    tio_tx_ops_t ops;

    TEST_CASE("terminal send status");
    sim_init(&sim, &ops);
    tio_tx_sm_reset(&sm);
    sim.busy_from_ms = 0;
    sim.busy_to_ms = 100;
    sim.send_fails = true;
    sim_enqueue(&sim, 0);

    sim.now_ms = 0;
    CHECK(tio_tx_sm_step(&sm, &ops, true));
    CHECK_EQ(sim.drops, 1);
    CHECK_EQ(sim.retries, 0);
    CHECK(!sm.pending);
}

int
main(void)
{
    test_short_busy_window();
    test_long_busy_window();
    test_watermark_release();
    test_disconnect_drops_held_packet();
    test_drain_without_usb();
    test_terminal_status_drops();
    return TEST_RESULT();
}
