
/**
 * @file ringbuffer.c
 * @author Adam Page (adam.page@ambiq.com)
 * @brief Basic ring buffer implementation
 * @version 1.1
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2023
 *
 * head/tail run over [0, 2*size) so that a full ring is representable:
 * head == tail means empty, head == tail + size (mod 2*size) means full.
 * See ringbuffer.h for the capacity and SPSC concurrency contract.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ringbuffer.h"

/**
 * @brief Fold an index in [0, 2*size) down to a storage index in [0, size)
 */
static inline uint32_t rb_index(const rb_config_t *ctx, uint32_t idx) {
    return idx >= ctx->size ? idx - ctx->size : idx;
}

/**
 * @brief Advance an index in [0, 2*size) by amt (amt <= size), wrapping at 2*size
 */
static inline uint32_t rb_advance(const rb_config_t *ctx, uint32_t idx, uint32_t amt) {
    uint32_t limit = ctx->size * 2u;
    uint32_t next = idx + amt;
    return next >= limit ? next - limit : next;
}

static inline void *rb_slot(const rb_config_t *ctx, uint32_t idx) {
    return ((char *)ctx->buffer) + (size_t)rb_index(ctx, idx) * ctx->dlen;
}

size_t ringbuffer_len(rb_config_t *ctx) {
    if (ctx->head >= ctx->tail) {
        return ctx->head - ctx->tail;
    }
    return (size_t)ctx->size * 2u - ctx->tail + ctx->head;
}

size_t ringbuffer_space(rb_config_t *ctx) {
    return ctx->size - ringbuffer_len(ctx);
}

size_t ringbuffer_capacity(rb_config_t *ctx) {
    return ctx->size;
}

size_t ringbuffer_push(rb_config_t *ctx, void *data, size_t len) {
    size_t space = ringbuffer_space(ctx);
    size_t amt = len > space ? space : len;
    for (size_t i = 0; i < amt; i++) {
        memcpy(rb_slot(ctx, ctx->head), ((char *)data) + i * ctx->dlen, ctx->dlen);
        ctx->head = rb_advance(ctx, ctx->head, 1);
    }
    return amt;
}

size_t ringbuffer_push_overwrite(rb_config_t *ctx, void *data, size_t len, size_t *evicted) {
    size_t dropped = 0;
    if (ctx->size == 0) {
        if (evicted) {
            *evicted = 0;
        }
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (ringbuffer_space(ctx) == 0) {
            // Drop-oldest: make room by discarding the element at the tail.
            ctx->tail = rb_advance(ctx, ctx->tail, 1);
            dropped++;
        }
        memcpy(rb_slot(ctx, ctx->head), ((char *)data) + i * ctx->dlen, ctx->dlen);
        ctx->head = rb_advance(ctx, ctx->head, 1);
    }
    if (evicted) {
        *evicted = dropped;
    }
    return len;
}

size_t ringbuffer_fill(rb_config_t *ctx, void *value, size_t len) {
    size_t space = ringbuffer_space(ctx);
    size_t amt = len > space ? space : len;
    for (size_t i = 0; i < amt; i++) {
        memcpy(rb_slot(ctx, ctx->head), value, ctx->dlen);
        ctx->head = rb_advance(ctx, ctx->head, 1);
    }
    return amt;
}

size_t ringbuffer_pop(rb_config_t *ctx, void *data, size_t len) {
    size_t avail = ringbuffer_len(ctx);
    size_t amt = len > avail ? avail : len;
    for (size_t i = 0; i < amt; i++) {
        memcpy(((char *)data) + i * ctx->dlen, rb_slot(ctx, ctx->tail), ctx->dlen);
        ctx->tail = rb_advance(ctx, ctx->tail, 1);
    }
    return amt;
}

size_t
ringbuffer_peek(rb_config_t *ctx, void *data, size_t len) {
    size_t avail = ringbuffer_len(ctx);
    size_t amt = len > avail ? avail : len;
    uint32_t tail = ctx->tail;
    for (size_t i = 0; i < amt; i++) {
        memcpy(((char *)data) + i * ctx->dlen, rb_slot(ctx, tail), ctx->dlen);
        tail = rb_advance(ctx, tail, 1);
    }
    return amt;
}

size_t
ringbuffer_seek(rb_config_t *ctx, size_t len) {
    size_t avail = ringbuffer_len(ctx);
    size_t amt = len > avail ? avail : len;
    ctx->tail = rb_advance(ctx, ctx->tail, (uint32_t)amt);
    return amt;
}

size_t
ringbuffer_transfer(rb_config_t *src, rb_config_t *dst, size_t len) {
    if (src->dlen != dst->dlen) {
        return 0;
    }
    size_t avail = ringbuffer_len(src);
    size_t space = ringbuffer_space(dst);
    size_t amt = len > avail ? avail : len;
    amt = amt > space ? space : amt;
    for (size_t i = 0; i < amt; i++) {
        memcpy(rb_slot(dst, dst->head), rb_slot(src, src->tail), src->dlen);
        src->tail = rb_advance(src, src->tail, 1);
        dst->head = rb_advance(dst, dst->head, 1);
    }
    return amt;
}

size_t
ringbuffer_flush(rb_config_t *ctx) {
    size_t len = ringbuffer_len(ctx);
    ctx->tail = ctx->head;
    return len;
}
