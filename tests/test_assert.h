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

/* 1 for a floating operand, 0 otherwise. The controlling expression of
 * _Generic is unevaluated, so this is a constant expression usable in
 * _Static_assert and costs nothing at runtime. */
#define HKV_IS_FLOATING(x) _Generic((x), float: 1, double: 1, long double: 1, default: 0)

/* Integer equality. The long long cast would compare only the integer part of
 * a floating operand, so the assert below rejects those at compile time. See #53. */
#define CHECK_EQ(actual, expected)                                                                                     \
    do {                                                                                                               \
        _Static_assert(!HKV_IS_FLOATING(actual) && !HKV_IS_FLOATING(expected),                                         \
                       "CHECK_EQ truncates to long long: use CHECK_FEQ or CHECK_NEAR for float or double");            \
        long long check_a_ = (long long)(actual);                                                                      \
        long long check_e_ = (long long)(expected);                                                                    \
        if (check_a_ != check_e_) {                                                                                    \
            TEST_FAIL("%s == %s (got %lld, want %lld)", #actual, #expected, check_a_, check_e_);                        \
        }                                                                                                              \
    } while (0)

/* Exact binary equality, for values the code under test computes identically to
 * the expected value. Widening to long double is exact for every floating type,
 * and %.21Lg round-trips it, so a failure prints the real values. NaN fails. */
#define CHECK_FEQ(actual, expected)                                                                                    \
    do {                                                                                                               \
        long double check_a_ = (long double)(actual);                                                                  \
        long double check_e_ = (long double)(expected);                                                                \
        if (!(check_a_ == check_e_)) {                                                                                 \
            TEST_FAIL("%s == %s (got %.21Lg, want %.21Lg)", #actual, #expected, check_a_, check_e_);                    \
        }                                                                                                              \
    } while (0)

/* Absolute tolerance, for values reached by a different route than the expected
 * value. Written as negated comparisons so NaN fails rather than passes. */
#define CHECK_NEAR(actual, expected, tol)                                                                              \
    do {                                                                                                               \
        long double check_a_ = (long double)(actual);                                                                  \
        long double check_e_ = (long double)(expected);                                                                \
        long double check_t_ = (long double)(tol);                                                                     \
        long double check_d_ = check_a_ - check_e_;                                                                    \
        if (!(check_d_ <= check_t_ && -check_d_ <= check_t_)) {                                                         \
            TEST_FAIL("%s == %s +/- %s (got %.21Lg, want %.21Lg, diff %.21Lg)", #actual, #expected, #tol, check_a_,     \
                      check_e_, check_d_);                                                                             \
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
