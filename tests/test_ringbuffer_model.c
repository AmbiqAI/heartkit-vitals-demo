// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file test_ringbuffer_model.c
 * @brief Differential test: the ringbuffer against a trivially-correct FIFO
 *        model, over a deterministic pseudo-random operation sequence.
 *
 * Closes two coverage gaps the hand-written tests leave open:
 *   - non-power-of-two sizes. The index arithmetic is correct by construction
 *     for any size, but the real rings are 256/500/2000, so the sizes here
 *     include 3, 5, 200 and 500 as well as a power of two.
 *   - ringbuffer_seek(), which is the only function that advances an index by
 *     a multi-element amount in a single rb_advance() call and which has
 *     several call sites in main.cc, yet had no direct test.
 *
 * The model is an array plus a count, which cannot express the "full is
 * indistinguishable from empty" bug by construction. Any divergence in a
 * return value, in len(), or in the bytes read back is a ringbuffer defect.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ringbuffer.h"
#include "test_assert.h"

#define MODEL_MAX 512

typedef struct {
    int32_t data[MODEL_MAX];
    size_t count;
    size_t cap;
} model_t;

static void model_init(model_t *m, size_t cap) {
    m->count = 0;
    m->cap = cap;
}

static void model_drop_front(model_t *m, size_t n) {
    memmove(m->data, m->data + n, (m->count - n) * sizeof(int32_t));
    m->count -= n;
}

static size_t model_push(model_t *m, const int32_t *src, size_t n) {
    size_t space = m->cap - m->count;
    size_t amt = n > space ? space : n;
    memcpy(m->data + m->count, src, amt * sizeof(int32_t));
    m->count += amt;
    return amt;
}

static size_t model_push_overwrite(model_t *m, const int32_t *src, size_t n, size_t *evicted) {
    size_t dropped = 0;
    for (size_t i = 0; i < n; i++) {
        if (m->count == m->cap) {
            model_drop_front(m, 1);
            dropped++;
        }
        m->data[m->count++] = src[i];
    }
    *evicted = dropped;
    return n;
}

static size_t model_peek(const model_t *m, int32_t *dst, size_t n) {
    size_t amt = n > m->count ? m->count : n;
    memcpy(dst, m->data, amt * sizeof(int32_t));
    return amt;
}

static size_t model_pop(model_t *m, int32_t *dst, size_t n) {
    size_t amt = model_peek(m, dst, n);
    model_drop_front(m, amt);
    return amt;
}

static size_t model_seek(model_t *m, size_t n) {
    size_t amt = n > m->count ? m->count : n;
    model_drop_front(m, amt);
    return amt;
}

static size_t model_fill(model_t *m, int32_t value, size_t n) {
    size_t space = m->cap - m->count;
    size_t amt = n > space ? space : n;
    for (size_t i = 0; i < amt; i++) {
        m->data[m->count + i] = value;
    }
    m->count += amt;
    return amt;
}

static size_t model_flush(model_t *m) {
    size_t c = m->count;
    m->count = 0;
    return c;
}

static uint32_t next_rand(uint32_t *state) {
    *state = (*state * 1103515245u) + 12345u;
    return (*state >> 16) & 0x7fffu;
}

/* Drive the same op sequence into the ring and the model, comparing after
 * every operation. */
static void run_model_sequence(size_t cap, uint32_t seed, int iters) {
    static char case_name[64];
    static int32_t storage[MODEL_MAX];
    static int32_t src[MODEL_MAX];
    static int32_t got[MODEL_MAX];
    static int32_t want[MODEL_MAX];
    model_t model;
    rb_config_t rb;
    uint32_t rng = seed;
    int32_t counter = 0;

    snprintf(case_name, sizeof(case_name), "model_sequence(cap=%zu,seed=%u)", cap, seed);
    TEST_CASE(case_name);

    memset(storage, 0, sizeof(storage));
    model_init(&model, cap);
    rb.buffer = storage;
    rb.dlen = sizeof(int32_t);
    rb.size = (uint32_t)cap;
    rb.head = 0;
    rb.tail = 0;

    CHECK_EQ(ringbuffer_capacity(&rb), cap);

    for (int iter = 0; iter < iters; iter++) {
        /* Request sizes deliberately exceed capacity sometimes. */
        size_t n = 1 + (size_t)(next_rand(&rng) % (uint32_t)(cap + 2));
        uint32_t op = next_rand(&rng) % 100u;

        if (op < 34) {
            for (size_t i = 0; i < n; i++) {
                src[i] = counter++;
            }
            CHECK_EQ(ringbuffer_push(&rb, src, n), model_push(&model, src, n));
        } else if (op < 44) {
            size_t rb_evicted = 0;
            size_t model_evicted = 0;
            for (size_t i = 0; i < n; i++) {
                src[i] = counter++;
            }
            CHECK_EQ(ringbuffer_push_overwrite(&rb, src, n, &rb_evicted),
                     model_push_overwrite(&model, src, n, &model_evicted));
            CHECK_EQ(rb_evicted, model_evicted);
        } else if (op < 70) {
            size_t want_n = model_pop(&model, want, n);
            size_t got_n = ringbuffer_pop(&rb, got, n);
            CHECK_EQ(got_n, want_n);
            if (got_n == want_n && got_n > 0) {
                CHECK_MEM(got, want, got_n * sizeof(int32_t));
            }
        } else if (op < 82) {
            size_t want_n = model_peek(&model, want, n);
            size_t got_n = ringbuffer_peek(&rb, got, n);
            CHECK_EQ(got_n, want_n);
            if (got_n == want_n && got_n > 0) {
                CHECK_MEM(got, want, got_n * sizeof(int32_t));
            }
        } else if (op < 92) {
            CHECK_EQ(ringbuffer_seek(&rb, n), model_seek(&model, n));
        } else if (op < 97) {
            int32_t value = counter++;
            CHECK_EQ(ringbuffer_fill(&rb, &value, n), model_fill(&model, value, n));
        } else {
            CHECK_EQ(ringbuffer_flush(&rb), model_flush(&model));
        }

        CHECK_EQ(ringbuffer_len(&rb), model.count);
        CHECK_EQ(ringbuffer_space(&rb), model.cap - model.count);
        CHECK_EQ(ringbuffer_len(&rb) + ringbuffer_space(&rb), cap);

        if (g_test_failures > 0) {
            fprintf(stderr, "  (diverged at iteration %d, op=%u, n=%zu)\n", iter, op, n);
            return; /* stop at first divergence; later output would be noise */
        }
    }
}

/* seek() must clamp to the available length and leave the oldest survivor at
 * the front, including when the skip crosses the storage wrap. */
static void test_seek_clamps_and_preserves_order(void) {
    TEST_CASE("seek_clamps_and_preserves_order");
    int32_t storage[8];
    int32_t src[8];
    int32_t out[8];
    int32_t want[3] = {5, 6, 7};
    rb_config_t rb = {.buffer = storage, .dlen = sizeof(int32_t), .size = 8, .head = 0, .tail = 0};

    for (int32_t i = 0; i < 8; i++) {
        src[i] = i;
    }
    CHECK_EQ(ringbuffer_push(&rb, src, 8), 8);

    CHECK_EQ(ringbuffer_seek(&rb, 5), 5);
    CHECK_EQ(ringbuffer_len(&rb), 3);
    CHECK_EQ(ringbuffer_peek(&rb, out, 8), 3);
    CHECK_MEM(out, want, sizeof(want));

    /* Over-seek clamps to what is there and empties the ring. */
    CHECK_EQ(ringbuffer_seek(&rb, 99), 3);
    CHECK_EQ(ringbuffer_len(&rb), 0);
    CHECK_EQ(ringbuffer_seek(&rb, 1), 0);
    CHECK_EQ(ringbuffer_space(&rb), 8);
}

/* A torn index pair (tail past head) must fail CLOSED. This models the
 * flush-racing-seek/pop hazard documented in main.cc: without the clamp in
 * ringbuffer_len() the raw length is bogus and huge, space() underflows to
 * ~SIZE_MAX, and push() would write far past the end of the backing array. */
static void test_inconsistent_indices_fail_closed(void) {
    TEST_CASE("inconsistent_indices_fail_closed");
    int32_t storage[8];
    int32_t canary[8];
    int32_t src[40];
    rb_config_t rb = {.buffer = storage, .dlen = sizeof(int32_t), .size = 8, .head = 0, .tail = 0};

    for (int i = 0; i < 8; i++) {
        storage[i] = -42;
        canary[i] = -42;
    }
    for (int i = 0; i < 40; i++) {
        src[i] = 1000 + i;
    }

    /* Torn state: tail ahead of head. */
    rb.head = 2;
    rb.tail = 5;

    CHECK(ringbuffer_len(&rb) <= 8);
    CHECK(ringbuffer_space(&rb) <= 8);
    CHECK_EQ(ringbuffer_len(&rb) + ringbuffer_space(&rb), 8);

    /* The critical assertion: a 40-element push into an 8-element ring must be
     * bounded by capacity, never accepted wholesale. */
    size_t pushed = ringbuffer_push(&rb, src, 40);
    CHECK(pushed <= 8);
    CHECK_EQ(ringbuffer_pop(&rb, src, 40) <= 8, 1);
    CHECK_EQ(ringbuffer_peek(&rb, src, 40) <= 8, 1);
    CHECK_EQ(ringbuffer_seek(&rb, 40) <= 8, 1);

    /* Recovery: flush restores a consistent empty ring. */
    ringbuffer_flush(&rb);
    CHECK_EQ(ringbuffer_len(&rb), 0);
    CHECK_EQ(ringbuffer_space(&rb), 8);
    (void)canary;
}

int main(void) {
    test_seek_clamps_and_preserves_order();
    test_inconsistent_indices_fail_closed();

    /* Power of two, small primes, and sizes near the real rings (256/500). */
    const size_t caps[] = {1, 2, 3, 5, 7, 8, 13, 200, 500};
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        for (uint32_t seed = 1; seed <= 3; seed++) {
            run_model_sequence(caps[i], seed, 400);
        }
    }
    return TEST_RESULT();
}
