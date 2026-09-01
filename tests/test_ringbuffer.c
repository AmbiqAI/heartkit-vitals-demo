/**
 * @file test_ringbuffer.c
 * @brief Host tests for the core ringbuffer contract.
 *
 * These tests use only the pre-existing ringbuffer API so that they can be
 * built and run against the *old* implementation to demonstrate the
 * full-buffer representation bug (see issue #10).
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

static void ring_init(test_ring_t *r, uint32_t size) {
    for (uint32_t i = 0; i < RB_SIZE; i++) {
        r->storage[i] = -1;
    }
    r->rb.buffer = r->storage;
    r->rb.dlen = sizeof(int32_t);
    r->rb.size = size;
    r->rb.head = 0;
    r->rb.tail = 0;
}

static void fill_seq(int32_t *dst, int32_t start, size_t n) {
    for (size_t i = 0; i < n; i++) {
        dst[i] = start + (int32_t)i;
    }
}

/* Filling to exactly capacity must report len == capacity and space == 0. */
static void test_fill_to_capacity(void) {
    TEST_CASE("fill_to_capacity");
    test_ring_t r;
    int32_t src[RB_SIZE];
    ring_init(&r, RB_SIZE);
    fill_seq(src, 0, RB_SIZE);

    CHECK_EQ(ringbuffer_push(&r.rb, src, RB_SIZE), RB_SIZE);
    CHECK_EQ(ringbuffer_len(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_space(&r.rb), 0);
}

/* A push against a genuinely full buffer must be a reportable short write and
 * must not disturb the stored contents. */
static void test_push_at_full_reports_short_write(void) {
    TEST_CASE("push_at_full_reports_short_write");
    test_ring_t r;
    int32_t src[RB_SIZE];
    int32_t extra[3] = {100, 101, 102};
    int32_t out[RB_SIZE];
    int32_t want[RB_SIZE];
    ring_init(&r, RB_SIZE);
    fill_seq(src, 0, RB_SIZE);
    fill_seq(want, 0, RB_SIZE);

    CHECK_EQ(ringbuffer_push(&r.rb, src, RB_SIZE), RB_SIZE);
    CHECK_EQ(ringbuffer_push(&r.rb, extra, 3), 0);
    CHECK_EQ(ringbuffer_len(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_peek(&r.rb, out, RB_SIZE), RB_SIZE);
    CHECK_MEM(out, want, sizeof(want));
}

/* After the head wraps past the end of storage, peek/pop must still return the
 * oldest element, not whatever happens to sit at index 0. */
static void test_peek_pop_after_wrap_returns_oldest(void) {
    TEST_CASE("peek_pop_after_wrap_returns_oldest");
    test_ring_t r;
    int32_t first[6];
    int32_t second[4];
    int32_t out[RB_SIZE];
    int32_t want[6] = {4, 5, 6, 7, 8, 9};
    ring_init(&r, RB_SIZE);
    fill_seq(first, 0, 6);
    fill_seq(second, 6, 4);

    CHECK_EQ(ringbuffer_push(&r.rb, first, 6), 6);
    CHECK_EQ(ringbuffer_pop(&r.rb, out, 4), 4);
    /* head is at 6; pushing 4 more wraps it back through 0. */
    CHECK_EQ(ringbuffer_push(&r.rb, second, 4), 4);
    CHECK_EQ(ringbuffer_len(&r.rb), 6);

    CHECK_EQ(ringbuffer_peek(&r.rb, out, 6), 6);
    CHECK_MEM(out, want, sizeof(want));
    CHECK_EQ(ringbuffer_len(&r.rb), 6); /* peek must not consume */
    CHECK_EQ(ringbuffer_pop(&r.rb, out, 6), 6);
    CHECK_MEM(out, want, sizeof(want));
    CHECK_EQ(ringbuffer_len(&r.rb), 0);
}

/* An oversized push across a wrap must stop at capacity and report the short
 * write. Against the old implementation this is the silent-wipe path: head
 * laps tail and len collapses towards zero. */
static void test_overflowing_wrap_does_not_wipe(void) {
    TEST_CASE("overflowing_wrap_does_not_wipe");
    test_ring_t r;
    int32_t first[6];
    int32_t second[7];
    int32_t out[RB_SIZE];
    int32_t want[RB_SIZE] = {4, 5, 6, 7, 8, 9, 10, 11};
    ring_init(&r, RB_SIZE);
    fill_seq(first, 0, 6);
    fill_seq(second, 6, 7);

    CHECK_EQ(ringbuffer_push(&r.rb, first, 6), 6);
    CHECK_EQ(ringbuffer_pop(&r.rb, out, 4), 4);
    CHECK_EQ(ringbuffer_len(&r.rb), 2);

    /* Only 6 slots free; the 7th element must be refused, not silently lap. */
    CHECK_EQ(ringbuffer_push(&r.rb, second, 7), 6);
    CHECK_EQ(ringbuffer_len(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_space(&r.rb), 0);
    CHECK_EQ(ringbuffer_peek(&r.rb, out, RB_SIZE), RB_SIZE);
    CHECK_MEM(out, want, sizeof(want));
}

/* transfer() must stop at the destination's free space and leave the
 * untransferred elements in the source. */
static void test_transfer_respects_destination_space(void) {
    TEST_CASE("transfer_respects_destination_space");
    test_ring_t src;
    test_ring_t dst;
    int32_t data[RB_SIZE];
    int32_t seed = 99;
    int32_t out[RB_SIZE];
    int32_t want_dst[4] = {99, 0, 1, 2};
    int32_t want_src[5] = {3, 4, 5, 6, 7};
    ring_init(&src, RB_SIZE);
    ring_init(&dst, 4);
    fill_seq(data, 0, RB_SIZE);

    CHECK_EQ(ringbuffer_push(&src.rb, data, RB_SIZE), RB_SIZE);
    CHECK_EQ(ringbuffer_push(&dst.rb, &seed, 1), 1);
    CHECK_EQ(ringbuffer_space(&dst.rb), 3);

    CHECK_EQ(ringbuffer_transfer(&src.rb, &dst.rb, RB_SIZE), 3);
    CHECK_EQ(ringbuffer_len(&dst.rb), 4);
    CHECK_EQ(ringbuffer_space(&dst.rb), 0);
    CHECK_EQ(ringbuffer_peek(&dst.rb, out, 4), 4);
    CHECK_MEM(out, want_dst, sizeof(want_dst));

    CHECK_EQ(ringbuffer_len(&src.rb), 5);
    CHECK_EQ(ringbuffer_peek(&src.rb, out, 5), 5);
    CHECK_MEM(out, want_src, sizeof(want_src));
}

/* flush() after a wrap must report the true length and leave the ring empty. */
static void test_flush_after_wrap(void) {
    TEST_CASE("flush_after_wrap");
    test_ring_t r;
    int32_t first[6];
    int32_t second[6];
    int32_t out[RB_SIZE];
    ring_init(&r, RB_SIZE);
    fill_seq(first, 0, 6);
    fill_seq(second, 6, 6);

    CHECK_EQ(ringbuffer_push(&r.rb, first, 6), 6);
    CHECK_EQ(ringbuffer_pop(&r.rb, out, 4), 4);
    CHECK_EQ(ringbuffer_push(&r.rb, second, 6), 6); /* wraps head */
    CHECK_EQ(ringbuffer_len(&r.rb), RB_SIZE);

    CHECK_EQ(ringbuffer_flush(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_len(&r.rb), 0);
    CHECK_EQ(ringbuffer_space(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_pop(&r.rb, out, 1), 0);
}

/* fill() must clamp to free space and report the clamped count. */
static void test_fill_clamps_to_space(void) {
    TEST_CASE("fill_clamps_to_space");
    test_ring_t r;
    int32_t value = 7;
    int32_t out[RB_SIZE];
    int32_t want[RB_SIZE] = {7, 7, 7, 7, 7, 7, 7, 7};
    ring_init(&r, RB_SIZE);

    CHECK_EQ(ringbuffer_fill(&r.rb, &value, RB_SIZE + 5), RB_SIZE);
    CHECK_EQ(ringbuffer_len(&r.rb), RB_SIZE);
    CHECK_EQ(ringbuffer_fill(&r.rb, &value, 1), 0);
    CHECK_EQ(ringbuffer_peek(&r.rb, out, RB_SIZE), RB_SIZE);
    CHECK_MEM(out, want, sizeof(want));
}

int main(void) {
    test_fill_to_capacity();
    test_push_at_full_reports_short_write();
    test_peek_pop_after_wrap_returns_oldest();
    test_overflowing_wrap_does_not_wipe();
    test_transfer_respects_destination_space();
    test_flush_after_wrap();
    test_fill_clamps_to_space();
    return TEST_RESULT();
}
