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

/* Sustained produce/consume across many wraps. The chunk size (3) is coprime
 * with both size (8) and the index domain (16), so this walks every index
 * alignment. Under the old implementation the ring silently emptied itself
 * once head lapped tail; here the FIFO order must hold indefinitely. */
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

int main(void) {
    test_capacity_equals_size();
    test_push_overwrite_no_eviction_when_space();
    test_push_overwrite_evicts_oldest();
    test_push_overwrite_longer_than_capacity();
    test_push_overwrite_null_evicted();
    test_sustained_wrap_preserves_fifo_order();
    test_stalled_consumer_reports_loss();
    return TEST_RESULT();
}
