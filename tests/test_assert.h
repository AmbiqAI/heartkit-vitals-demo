// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file test_assert.h
 * @brief Minimal host-test assertion macros (no framework, no dependencies).
 *
 * Each test file defines its own main() and returns TEST_RESULT(). The failure
 * COUNT is printed, but the exit status is only 0 or 1: an exit status is
 * truncated to 8 bits, so returning the raw count would make exactly 256
 * failures look like success.
 */
#ifndef HKV_TEST_ASSERT_H
#define HKV_TEST_ASSERT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int g_test_failures = 0;
static const char *g_test_name = "<none>";

#define TEST_CASE(name) (g_test_name = (name))

#define TEST_FAIL(fmt, ...)                                                                                            \
    do {                                                                                                               \
        g_test_failures++;                                                                                             \
        fprintf(stderr, "FAIL [%s] %s:%d: " fmt "\n", g_test_name, __FILE__, __LINE__, __VA_ARGS__);                    \
    } while (0)

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            TEST_FAIL("CHECK(%s)", #cond);                                                                             \
        }                                                                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                                                                     \
    do {                                                                                                               \
        long long check_a_ = (long long)(actual);                                                                      \
        long long check_e_ = (long long)(expected);                                                                    \
        if (check_a_ != check_e_) {                                                                                    \
            TEST_FAIL("%s == %s (got %lld, want %lld)", #actual, #expected, check_a_, check_e_);                        \
        }                                                                                                              \
    } while (0)

#define CHECK_MEM(actual, expected, nbytes)                                                                            \
    do {                                                                                                               \
        if (memcmp((actual), (expected), (nbytes)) != 0) {                                                             \
            TEST_FAIL("CHECK_MEM(%s, %s, %zu bytes)", #actual, #expected, (size_t)(nbytes));                            \
        }                                                                                                              \
    } while (0)

#define TEST_RESULT()                                                                                                  \
    (fprintf(stderr, "%s: %d check failure(s)\n", __FILE__, g_test_failures), g_test_failures ? 1 : 0)

#endif // HKV_TEST_ASSERT_H
