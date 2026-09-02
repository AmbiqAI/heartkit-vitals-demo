// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file ringbuffer.h
 * @author Adam Page (adam.page@ambiq.com)
 * @brief Basic ring buffer implementation
 * @version 1.1
 * @date 2023-12-13
 *
 * @copyright Copyright (c) 2023
 *
 * ## Capacity semantics
 *
 * A ring holds up to `size` elements: **capacity == size**, so the backing
 * array must have exactly `size` elements and no slot is reserved. A full ring
 * is representable: `ringbuffer_len()` returns `size` and
 * `ringbuffer_space()` returns 0. Short writes are therefore reportable --
 * `ringbuffer_push()`, `ringbuffer_fill()` and `ringbuffer_transfer()` return
 * a count smaller than requested when the destination runs out of room.
 *
 * This is achieved by letting `head`/`tail` run over `[0, 2*size)` rather than
 * `[0, size)`; the extra bit distinguishes "full" (head == tail + size) from
 * "empty" (head == tail). The storage index is the value folded back into
 * `[0, size)`. Treat `head`/`tail` as opaque: only zero-initialise them.
 *
 * Constraint: `size` must be <= RINGBUFFER_MAX_SIZE so that index arithmetic
 * cannot overflow. Every buffer in this project is orders of magnitude below
 * that.
 *
 * ## Concurrency: single-producer / single-consumer, lock free
 *
 * The ring is lock free for exactly one producer and one consumer, because
 * each index has a single writer:
 *   - producer writes `head` only: `ringbuffer_push()`, `ringbuffer_fill()`,
 *     and the destination side of `ringbuffer_transfer()`.
 *   - consumer writes `tail` only: `ringbuffer_pop()`, `ringbuffer_seek()`,
 *     `ringbuffer_flush()`, and the source side of `ringbuffer_transfer()`.
 * `ringbuffer_len()`/`ringbuffer_space()` only read both indices, so a stale
 * read is conservative: the producer may under-estimate free space and the
 * consumer may under-estimate available data, never the reverse.
 *
 * If the invariant is nevertheless violated (e.g. a `ringbuffer_flush()`
 * racing a `ringbuffer_seek()`/`ringbuffer_pop()` leaves tail past head),
 * `ringbuffer_len()` clamps to `size` so every operation fails CLOSED: the
 * ring reads as full or empty and no write can run past the backing array.
 *
 * There are no locks or critical sections by design: `ringbuffer_push()` is
 * called from the AS7058 interrupt service path, which has a bounded service
 * window and cannot afford them.
 *
 * EXCEPTION: `ringbuffer_push_overwrite()` writes BOTH indices and is
 * therefore NOT SPSC safe. Use it only where the producer is the sole mutator
 * of the ring, or where producer and consumer are externally serialised.
 */
#ifndef __RINGBUFFER_H
#define __RINGBUFFER_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>

/**
 * @brief Largest legal value for rb_config_t::size.
 *
 * Index arithmetic works in [0, 2*size) and transiently evaluates up to
 * 3*size, so size must stay below UINT32_MAX/3. UINT32_MAX/4 is the safer
 * round bound. Declarations should static_assert their size against this,
 * e.g. `_Static_assert(MY_BUF_LEN <= RINGBUFFER_MAX_SIZE, "ring too large");`
 */
#define RINGBUFFER_MAX_SIZE (UINT32_MAX / 4u)

typedef struct {
    void *buffer;    /**< Backing storage, must hold `size` elements of `dlen` bytes */
    size_t dlen;     /**< Size of one element in bytes */
    uint32_t size;   /**< Capacity in elements */
    uint32_t head;   /**< Producer index in [0, 2*size). Opaque; zero-initialise only */
    uint32_t tail;   /**< Consumer index in [0, 2*size). Opaque; zero-initialise only */
} rb_config_t;


/**
 * @brief Number of elements currently stored, in [0, size]
 *
 * @param ctx Ringbuffer context
 * @return size_t
 */
size_t
ringbuffer_len(rb_config_t *ctx);

/**
 * @brief Free space in elements, i.e. capacity - len. Returns 0 when full.
 *
 * @param ctx Ringbuffer context
 * @return size_t
 */
size_t
ringbuffer_space(rb_config_t *ctx);

/**
 * @brief Capacity in elements (equal to ctx->size)
 *
 * @param ctx Ringbuffer context
 * @return size_t
 */
size_t
ringbuffer_capacity(rb_config_t *ctx);

/**
 * @brief Push data to ringbuffer, refusing to overwrite unread data.
 *
 * Stops when the ring is full and reports the short write.
 *
 * @param ctx Ringbuffer context
 * @param data Data to push
 * @param len Number of elements to push
 * @return size_t Number of elements actually written; < len means the ring
 *         filled up and (len - return) elements were rejected.
 */
size_t
ringbuffer_push(rb_config_t *ctx, void *data, size_t len);

/**
 * @brief Push data to ringbuffer, evicting the oldest elements on overflow.
 *
 * Drop-oldest counterpart to ringbuffer_push(): all `len` elements are always
 * written, and the tail is advanced as needed to make room. The number of
 * evicted (lost) elements is reported so callers can maintain a loss counter.
 *
 * @warning Writes both head and tail, so this is NOT SPSC safe. See the
 *          concurrency notes at the top of this header.
 *
 * @param ctx Ringbuffer context
 * @param data Data to push
 * @param len Number of elements to push
 * @param evicted Optional out-param: number of old elements discarded
 * @return size_t Number of elements written (equals len unless size == 0)
 */
size_t
ringbuffer_push_overwrite(rb_config_t *ctx, void *data, size_t len, size_t *evicted);

/**
 * @brief Fill ringbuffer by repeating a single value, clamped to free space
 *
 * @param ctx Ringbuffer context
 * @param value Value to repeat
 * @param len Number of elements requested
 * @return size_t Number of elements actually written
 */
size_t
ringbuffer_fill(rb_config_t *ctx, void *value, size_t len);

/**
 * @brief Pop oldest data from ringbuffer, clamped to available length
 *
 * @param ctx Ringbuffer context
 * @param data Buffer to store data
 * @param len Number of elements requested
 * @return size_t Number of elements actually read
 */
size_t
ringbuffer_pop(rb_config_t *ctx, void *data, size_t len);

/**
 * @brief Read oldest data w/o removing (peek), clamped to available length
 *
 * @param ctx Ringbuffer context
 * @param data Buffer to store data
 * @param len Number of elements requested
 * @return size_t Number of elements actually read
 */
size_t
ringbuffer_peek(rb_config_t *ctx, void *data, size_t len);

/**
 * @brief Discard oldest elements without reading them
 *
 * @param ctx Ringbuffer context
 * @param len Number of elements requested
 * @return size_t Number of elements actually discarded
 */
size_t
ringbuffer_seek(rb_config_t *ctx, size_t len);

/**
 * @brief Move data from one ringbuffer to another
 *
 * Clamped to both the source length and the destination free space.
 *
 * @param src Source ringbuffer
 * @param dst Destination ringbuffer
 * @param len Number of elements requested
 * @return size_t Number of elements actually moved
 */
size_t
ringbuffer_transfer(rb_config_t *src, rb_config_t *dst, size_t len);

/**
 * @brief Discard all buffered elements
 *
 * @param ctx Ringbuffer context
 * @return size_t Number of elements discarded
 */
size_t
ringbuffer_flush(rb_config_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // __RINGBUFFER_H
