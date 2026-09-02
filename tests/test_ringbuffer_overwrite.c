// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file test_ringbuffer_overwrite.c
 * @brief Host tests for ringbuffer_push_overwrite() (drop-oldest) plus
 *        long-running wrap-around regression coverage for issue #10.
 */
#include <stdint.h>
#include <stddef.h>

#include "ringbuffer.h"
#include "test_assert.h"

#define RB_SIZE 8

typedef struct {
    int32_t storage[RB_SIZE];
    rb_config_t rb;
} test_ring_t;

static void ring_init(test_ring_t *r) {
    for (uint32_t i = 0; i < RB_SIZE; i++) {
        r->storage[i] = -1;
    }
    r->rb.buffer = r->storage;
    r->rb.dlen = sizeof(int32_t);
    r->rb.size = RB_SIZE;
    r->rb.head = 0;
    r->rb.tail = 0;
}

static void fill_seq(int32_t *dst, int32_t start, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = start + (int32_t)i;
    }
}

/* capacity is the declared size; no slot is reserved. */
static void test_capacity_equals_size(void) {
    TEST_CASE("capacity_equals_size");
    test_ring_t r;
    int32_t src[RB_SIZE];
    ring_init(&r);
    fill_seq(src, 0, RB_SIZE);

    CHECK_EQ(ringbuffer_capacity(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_space(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_push(&r.rb, src, RB_SIZE), RB_SIZE);
    CHECK_EQ(ringbuffer_capacity(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_len(&r.rb), ringbuffer_capacity(&r.rb));
}

/* With room available, push_overwrite behaves exactly like push. */
static void test_push_overwrite_no_eviction_when_space(void) {
    TEST_CASE("push_overwrite_no_eviction_when_space");
    test_ring_t r;
    int32_t src[5];
    int32_t out[RB_SIZE];
    int32_t want[5] = {0, 1, 2, 3, 4};
    size_t evicted = 12345;
    ring_init(&r);
    fill_seq(src, 0, 5);

    CHECK_EQ(ringbuffer_push_overwrite(&r.rb, src, 5, &evicted), 5);
    CHECK_EQ(evicted, 0);
    CHECK_EQ(ringbuffer_len(&r.rb), 5);
    CHECK_EQ(ringbuffer_peek(&r.rb, out, 5), 5);
    CHECK_MEM(out, want, sizeof(want));
}

/* At full, push_overwrite drops the oldest elements and reports how many. */
static void test_push_overwrite_evicts_oldest(void) {
    TEST_CASE("push_overwrite_evicts_oldest");
    test_ring_t r;
    int32_t src[RB_SIZE];
    int32_t extra[3] = {100, 101, 102};
    int32_t out[RB_SIZE];
    int32_t want[RB_SIZE] = {3, 4, 5, 6, 7, 100, 101, 102};
    size_t evicted = 12345;
    ring_init(&r);
    fill_seq(src, 0, RB_SIZE);

    CHECK_EQ(ringbuffer_push(&r.rb, src, RB_SIZE), RB_SIZE);
    CHECK_EQ(ringbuffer_space(&r.rb), 0);

    CHECK_EQ(ringbuffer_push_overwrite(&r.rb, extra, 3, &evicted), 3);
    CHECK_EQ(evicted, 3);
    CHECK_EQ(ringbuffer_len(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_space(&r.rb), 0);
    CHECK_EQ(ringbuffer_peek(&r.rb, out, RB_SIZE), RB_SIZE);
    CHECK_MEM(out, want, sizeof(want));
}

/* A single write longer than the whole ring keeps the newest `size` elements
 * and counts every discarded element. */
static void test_push_overwrite_longer_than_capacity(void) {
    TEST_CASE("push_overwrite_longer_than_capacity");
    test_ring_t r;
    int32_t src[12];
    int32_t out[RB_SIZE];
    int32_t want[RB_SIZE] = {4, 5, 6, 7, 8, 9, 10, 11};
    size_t evicted = 12345;
    ring_init(&r);
    fill_seq(src, 0, 12);

    CHECK_EQ(ringbuffer_push_overwrite(&r.rb, src, 12, &evicted), 12);
    CHECK_EQ(evicted, 4);
    CHECK_EQ(ringbuffer_len(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_peek(&r.rb, out, RB_SIZE), RB_SIZE);
    CHECK_MEM(out, want, sizeof(want));
}

/* The evicted out-param is optional. */
static void test_push_overwrite_null_evicted(void) {
    TEST_CASE("push_overwrite_null_evicted");
    test_ring_t r;
    int32_t src[12];
    int32_t out[RB_SIZE];
    int32_t want[RB_SIZE] = {4, 5, 6, 7, 8, 9, 10, 11};
    ring_init(&r);
    fill_seq(src, 0, 12);

    CHECK_EQ(ringbuffer_push_overwrite(&r.rb, src, 12, NULL), 12);
    CHECK_EQ(ringbuffer_len(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_peek(&r.rb, out, RB_SIZE), RB_SIZE);
    CHECK_MEM(out, want, sizeof(want));
}

/* Walks every index alignment. The chunk size (3) is coprime with both the
 * size (8) and the index domain (16), so head/tail land on every residue and
 * the [0, 2*size) wrap arithmetic gets exercised at each one.
 *
 * Scope note: occupancy never exceeds 3 of 8 here, so the ring never fills and
 * this does NOT exercise the lapping bug -- it passes against the old
 * implementation too. It is index-arithmetic coverage only. High-occupancy
 * behaviour is covered by test_sustained_high_occupancy() below. */
static void test_sustained_wrap_preserves_fifo_order(void) {
    TEST_CASE("sustained_wrap_preserves_fifo_order");
    test_ring_t r;
    int32_t chunk[3];
    int32_t out[3];
    int32_t next_write = 0;
    int32_t next_read = 0;
    ring_init(&r);

    for (int iter = 0; iter < 200; iter++) {
        fill_seq(chunk, next_write, 3);
        CHECK_EQ(ringbuffer_push(&r.rb, chunk, 3), 3);
        next_write += 3;
        CHECK(ringbuffer_len(&r.rb) <= RB_SIZE);
        CHECK_EQ(ringbuffer_len(&r.rb) + ringbuffer_space(&r.rb), RB_SIZE);

        CHECK_EQ(ringbuffer_pop(&r.rb, out, 3), 3);
        for (int i = 0; i < 3; i++) {
            CHECK_EQ(out[i], next_read + i);
        }
        next_read += 3;
    }
    CHECK_EQ(ringbuffer_len(&r.rb), 0);
    CHECK_EQ(next_read, 600);
}

/* A stalled consumer must cause reported short writes, never a silent wipe. */
static void test_stalled_consumer_reports_loss(void) {
    TEST_CASE("stalled_consumer_reports_loss");
    test_ring_t r;
    int32_t chunk[4];
    int32_t out[RB_SIZE];
    int32_t want[RB_SIZE] = {0, 1, 2, 3, 4, 5, 6, 7};
    int32_t next_write = 0;
    size_t total_requested = 0;
    size_t total_pushed = 0;
    ring_init(&r);

    /* Consumer never runs: 5 chunks of 4 into a ring of 8. */
    for (int iter = 0; iter < 5; iter++) {
        fill_seq(chunk, next_write, 4);
        total_pushed += ringbuffer_push(&r.rb, chunk, 4);
        total_requested += 4;
        next_write += 4;
        CHECK_EQ(ringbuffer_len(&r.rb), (iter < 2) ? (size_t)(4 * (iter + 1)) : (size_t)RB_SIZE);
    }
    CHECK_EQ(total_requested, 20);
    CHECK_EQ(total_pushed, 8);
    CHECK_EQ(total_requested - total_pushed, 12); /* the drop counter callers keep */

    /* Refusing-at-full semantics: the OLDEST data survives intact. */
    CHECK_EQ(ringbuffer_peek(&r.rb, out, RB_SIZE), RB_SIZE);
    CHECK_MEM(out, want, sizeof(want));
}

/* Sustained operation AT capacity, which is where the old implementation
 * broke. Phase 1 pushes 5 and pops 3 so occupancy ratchets up to full and the
 * producer starts taking real short writes; phase 2 then runs balanced 3/3
 * traffic while the ring stays saturated. Throughout, whatever the ring
 * accepted must come back out in FIFO order with nothing skipped or
 * duplicated -- the old code silently discarded the entire contents once head
 * lapped tail. */
static void test_sustained_high_occupancy(void) {
    TEST_CASE("sustained_high_occupancy");
    test_ring_t r;
    int32_t chunk[5];
    int32_t out[5];
    /* Shadow FIFO of exactly what the ring accepted. Short writes create gaps
     * in the value stream, so the expected output is NOT contiguous and has to
     * be modelled rather than computed. */
    int32_t expect[RB_SIZE];
    size_t expect_count = 0;
    int32_t next_write = 0;
    size_t total_offered = 0;
    size_t total_accepted = 0;
    size_t total_read = 0;
    int saw_short_write = 0;
    int saw_full = 0;
    ring_init(&r);

#define SHADOW_PUSH(src, n)                                                                                            \
    do {                                                                                                               \
        for (size_t si = 0; si < (n); si++) {                                                                          \
            expect[expect_count++] = (src)[si];                                                                        \
        }                                                                                                              \
    } while (0)

#define SHADOW_POP(got, n)                                                                                             \
    do {                                                                                                               \
        CHECK(expect_count >= (n));                                                                                    \
        for (size_t si = 0; si < (n); si++) {                                                                          \
            CHECK_EQ((got)[si], expect[si]);                                                                           \
        }                                                                                                              \
        memmove(expect, expect + (n), (expect_count - (n)) * sizeof(int32_t));                                         \
        expect_count -= (n);                                                                                           \
    } while (0)

    /* Phase 1: producer outruns consumer, occupancy ratchets up to full. */
    for (int iter = 0; iter < 20; iter++) {
        fill_seq(chunk, next_write, 5);
        size_t pushed = ringbuffer_push(&r.rb, chunk, 5);
        if (pushed < 5) {
            saw_short_write = 1;
        }
        SHADOW_PUSH(chunk, pushed);
        total_offered += 5;
        total_accepted += pushed;
        next_write += 5;
        CHECK(ringbuffer_len(&r.rb) <= RB_SIZE);
        CHECK_EQ(ringbuffer_len(&r.rb), expect_count);
        CHECK_EQ(ringbuffer_len(&r.rb) + ringbuffer_space(&r.rb), RB_SIZE);
        if (ringbuffer_len(&r.rb) == RB_SIZE) {
            saw_full = 1; /* saturation happens mid-cycle, right after the push */
        }

        size_t popped = ringbuffer_pop(&r.rb, out, 3);
        SHADOW_POP(out, popped);
        total_read += popped;
        CHECK_EQ(ringbuffer_len(&r.rb), expect_count);
    }
    CHECK(saw_short_write); /* the whole point: loss is reportable, not silent */
    CHECK(saw_full);        /* and the ring genuinely reached capacity */

    /* Phase 2: balanced 3-in/3-out traffic at the high-occupancy fixed point
     * the ratchet settled on. Occupancy oscillates just below capacity, so the
     * head/tail pair keeps crossing the storage wrap under load. */
    for (int iter = 0; iter < 50; iter++) {
        size_t popped = ringbuffer_pop(&r.rb, out, 3);
        CHECK_EQ(popped, 3);
        SHADOW_POP(out, popped);
        total_read += popped;

        fill_seq(chunk, next_write, 3);
        size_t pushed = ringbuffer_push(&r.rb, chunk, 3);
        CHECK_EQ(pushed, 3);
        SHADOW_PUSH(chunk, pushed);
        total_offered += 3;
        total_accepted += pushed;
        next_write += 3;
        CHECK_EQ(ringbuffer_len(&r.rb), expect_count);
        CHECK(ringbuffer_len(&r.rb) <= RB_SIZE);
        CHECK_EQ(ringbuffer_len(&r.rb) + ringbuffer_space(&r.rb), RB_SIZE);
    }

    /* Drain and confirm the ring gave back exactly what it accepted. */
    while (ringbuffer_len(&r.rb) > 0) {
        size_t popped = ringbuffer_pop(&r.rb, out, 3);
        CHECK(popped > 0);
        SHADOW_POP(out, popped);
        total_read += popped;
    }
    CHECK_EQ(expect_count, 0);
    CHECK_EQ(ringbuffer_len(&r.rb), 0);
    CHECK_EQ(total_read, total_accepted);
    CHECK(total_accepted < total_offered); /* some was refused, and we know how much */

#undef SHADOW_PUSH
#undef SHADOW_POP
}

int main(void) {
    test_capacity_equals_size();
    test_push_overwrite_no_eviction_when_space();
    test_push_overwrite_evicts_oldest();
    test_push_overwrite_longer_than_capacity();
    test_push_overwrite_null_evicted();
    test_sustained_wrap_preserves_fifo_order();
    test_sustained_high_occupancy();
    test_stalled_consumer_reports_loss();
    return TEST_RESULT();
}
