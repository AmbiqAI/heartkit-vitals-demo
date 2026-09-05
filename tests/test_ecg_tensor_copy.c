// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Ambiq
/**
 * @file test_ecg_tensor_copy.c
 * @brief Host tests for the ECG tensor copies (src/ecg_tensor_copy.h).
 *
 * Host buffers are malloc'd at their exact size so ASan red-zones them. See #36.
 */
#include <stdint.h>
#include <stdlib.h>

#include "ecg_tensor_copy.h"
#include "test_assert.h"

/* The deployed models are [1, 256, ...] and the host windows are now 256 wide,
 * so a deployed-width fixture would exercise an empty tail and prove nothing.
 * The fixture tensor stays deliberately wider than both host windows so the
 * host-side bound and the edge-replicated tail remain under test. See #36. */
#define MODEL_TAIL 6
#define HOST_WIDEST_LEN ((ECG_DEN_WINDOW_LEN > ECG_SEG_WINDOW_LEN) ? ECG_DEN_WINDOW_LEN : ECG_SEG_WINDOW_LEN)
#define MODEL_LEN (HOST_WIDEST_LEN + MODEL_TAIL)
#define MODEL_CLASSES ECG_SEG_NUM_CLASS

/* Exact powers of two, so every expected value below is representable and the
 * checks can use equality rather than a tolerance. */
#define Q_SCALE (1.0f / 128.0f)
#define Q_ZERO (-128)

/* Identity quantization on the input side: the firmware's float-to-int8 cast is
 * undefined for out-of-range values, so the fixture keeps every sample inside
 * int8 rather than testing that separate defect by accident. */
#define IN_SCALE (1.0f)
#define IN_ZERO (0)

#define HOST_SENTINEL (-999.0f)
#define MASK_SENTINEL (0xBEEFu)

/* Winning-class scores for the segmentation fixture, quantized by Q_SCALE and
 * Q_ZERO: 1.0 inside the host window, 0.5 on the tensor tail. */
#define SEG_WIN_QOS (1.0f)
#define SEG_TAIL_QOS (0.5f)
#define SEG_WIN_I8 ((int8_t)(SEG_WIN_QOS * 128.0f + (float)Q_ZERO))
#define SEG_TAIL_I8 ((int8_t)(SEG_TAIL_QOS * 128.0f + (float)Q_ZERO))
static void *
xalloc(size_t nbytes) {
    void *p = malloc(nbytes);
    if (p == NULL) { abort(); }
    return p;
}

static void
test_denoise_output_f32(void) {
    float *host = (float *)xalloc(ECG_DEN_WINDOW_LEN * sizeof(float));
    float *tensor = (float *)xalloc(MODEL_LEN * sizeof(float));

    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) { host[i] = HOST_SENTINEL; }
    for (int i = 0; i < MODEL_LEN; i++) { tensor[i] = (float)i; }

    /* THE OVERRUN. Bounded by the tensor this wrote past the end of host[]. */
    TEST_CASE("float output copy stops at the host window, not the tensor");
    hkv_tensor_output_f32(host, hkv_host_len(ECG_DEN_WINDOW_LEN), tensor, hkv_tensor_len(MODEL_LEN), 0);
    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) { CHECK_FEQ(host[i], (float)i); }

    TEST_CASE("float output copy leaves both pad regions untouched");
    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) { host[i] = HOST_SENTINEL; }
    hkv_tensor_output_f32(host, hkv_host_len(ECG_DEN_WINDOW_LEN), tensor, hkv_tensor_len(MODEL_LEN), ECG_DEN_PAD_LEN);
    for (int i = 0; i < ECG_DEN_PAD_LEN; i++) { CHECK_FEQ(host[i], HOST_SENTINEL); }
    for (int i = ECG_DEN_PAD_LEN; i < ECG_DEN_WINDOW_LEN - ECG_DEN_PAD_LEN; i++) { CHECK_FEQ(host[i], (float)i); }
    for (int i = ECG_DEN_WINDOW_LEN - ECG_DEN_PAD_LEN; i < ECG_DEN_WINDOW_LEN; i++) {
        CHECK_FEQ(host[i], HOST_SENTINEL);
    }

    free(host);
    free(tensor);
}

static void
test_denoise_output_i8(void) {
    float *host = (float *)xalloc(ECG_DEN_WINDOW_LEN * sizeof(float));
    int8_t *tensor = (int8_t *)xalloc(MODEL_LEN * sizeof(int8_t));

    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) { host[i] = HOST_SENTINEL; }
    for (int i = 0; i < MODEL_LEN; i++) { tensor[i] = (int8_t)(i - 128); }

    TEST_CASE("quantized output copy stops at the host window");
    hkv_tensor_output_i8(host, hkv_host_len(ECG_DEN_WINDOW_LEN), tensor, hkv_tensor_len(MODEL_LEN), 0, Q_SCALE, Q_ZERO);
    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) {
        CHECK_FEQ(host[i], (float)((int)tensor[i] - Q_ZERO) * Q_SCALE);
    }

    free(host);
    free(tensor);
}

static void
test_segmentation_output_mask(void) {
    uint16_t *mask = (uint16_t *)xalloc(ECG_SEG_WINDOW_LEN * sizeof(uint16_t));
    int8_t *tensor = (int8_t *)xalloc(MODEL_LEN * MODEL_CLASSES * sizeof(int8_t));
    float avgQos;

    for (int i = 0; i < ECG_SEG_WINDOW_LEN; i++) { mask[i] = MASK_SENTINEL; }
    /* One winning class per sample, every other class at 0.0. Rows inside the
     * host window win at 1.0 and rows past it win at 0.5, so the mean over the
     * copied range and the mean over the whole tensor are different numbers. */
    for (int i = 0; i < MODEL_LEN; i++) {
        for (int j = 0; j < MODEL_CLASSES; j++) { tensor[i * MODEL_CLASSES + j] = (int8_t)Q_ZERO; }
        tensor[i * MODEL_CLASSES + (i % MODEL_CLASSES)] = (i < ECG_SEG_WINDOW_LEN) ? SEG_WIN_I8 : SEG_TAIL_I8;
    }

    /* THE OVERRUN. Bounded by the tensor this wrote past the end of mask[]. */
    TEST_CASE("segmentation mask copy stops at the host window, not the tensor");
    avgQos = hkv_seg_output_mask(mask, hkv_host_len(ECG_SEG_WINDOW_LEN), tensor, NULL, hkv_tensor_len(MODEL_LEN),
                                 MODEL_CLASSES, 0, (float)ECG_SEG_THRESHOLD, Q_SCALE, Q_ZERO);
    for (int i = 0; i < ECG_SEG_WINDOW_LEN; i++) {
        CHECK_EQ(mask[i], (uint16_t)((i % MODEL_CLASSES) | (3u << SIG_MASK_QOS_OFFSET)));
    }
    /* The mean over the host window is 1.0, not the lower mean the tensor tail
     * would give. Both are exact in binary32, so this is an equality check. */
    TEST_CASE("segmentation qos averages over the copied range only");
    CHECK_FEQ(avgQos, SEG_WIN_QOS);

    TEST_CASE("segmentation mask copy leaves both pad regions untouched");
    for (int i = 0; i < ECG_SEG_WINDOW_LEN; i++) { mask[i] = MASK_SENTINEL; }
    (void)hkv_seg_output_mask(mask, hkv_host_len(ECG_SEG_WINDOW_LEN), tensor, NULL, hkv_tensor_len(MODEL_LEN),
                              MODEL_CLASSES, ECG_SEG_PAD_LEN, (float)ECG_SEG_THRESHOLD, Q_SCALE, Q_ZERO);
    for (int i = 0; i < ECG_SEG_PAD_LEN; i++) { CHECK_EQ(mask[i], MASK_SENTINEL); }
    for (int i = ECG_SEG_WINDOW_LEN - ECG_SEG_PAD_LEN; i < ECG_SEG_WINDOW_LEN; i++) { CHECK_EQ(mask[i], MASK_SENTINEL); }

    free(mask);
    free(tensor);
}

static void
test_segmentation_output_mask_f32(void) {
    uint16_t *mask = (uint16_t *)xalloc(ECG_SEG_WINDOW_LEN * sizeof(uint16_t));
    float *tensor = (float *)xalloc(MODEL_LEN * MODEL_CLASSES * sizeof(float));

    for (int i = 0; i < ECG_SEG_WINDOW_LEN; i++) { mask[i] = MASK_SENTINEL; }
    for (int i = 0; i < MODEL_LEN; i++) {
        for (int j = 0; j < MODEL_CLASSES; j++) { tensor[i * MODEL_CLASSES + j] = 0.0f; }
        tensor[i * MODEL_CLASSES + (i % MODEL_CLASSES)] = 1.0f;
    }

    TEST_CASE("float segmentation mask copy stops at the host window");
    (void)hkv_seg_output_mask(mask, hkv_host_len(ECG_SEG_WINDOW_LEN), NULL, tensor, hkv_tensor_len(MODEL_LEN),
                              MODEL_CLASSES, 0, (float)ECG_SEG_THRESHOLD, Q_SCALE, Q_ZERO);
    for (int i = 0; i < ECG_SEG_WINDOW_LEN; i++) {
        CHECK_EQ(mask[i], (uint16_t)((i % MODEL_CLASSES) | (3u << SIG_MASK_QOS_OFFSET)));
    }

    free(mask);
    free(tensor);
}

static void
test_input_tail_is_defined(void) {
    float *host = (float *)xalloc(ECG_DEN_WINDOW_LEN * sizeof(float));
    float *tensorF = (float *)xalloc(MODEL_LEN * sizeof(float));
    int8_t *tensorI8 = (int8_t *)xalloc(MODEL_LEN * sizeof(int8_t));

    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) { host[i] = (float)((i % 100) - 50); }

    /* THE ARENA RESIDUE. Poison stands in for whatever the previous inference
     * left in the arena: an untouched tail keeps it and feeds it to the model. */
    for (int i = 0; i < MODEL_LEN; i++) {
        tensorF[i] = HOST_SENTINEL;
        tensorI8[i] = 0x5A;
    }

    TEST_CASE("float input tail is edge replicated, not left as arena residue");
    hkv_tensor_input_f32(tensorF, hkv_tensor_len(MODEL_LEN), host, hkv_host_len(ECG_DEN_WINDOW_LEN));
    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) { CHECK_FEQ(tensorF[i], host[i]); }
    for (int i = ECG_DEN_WINDOW_LEN; i < MODEL_LEN; i++) { CHECK_FEQ(tensorF[i], host[ECG_DEN_WINDOW_LEN - 1]); }

    TEST_CASE("quantized input tail is edge replicated");
    hkv_tensor_input_i8(tensorI8, hkv_tensor_len(MODEL_LEN), host, hkv_host_len(ECG_DEN_WINDOW_LEN), IN_SCALE, IN_ZERO);
    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) { CHECK_EQ(tensorI8[i], (int8_t)host[i]); }
    for (int i = ECG_DEN_WINDOW_LEN; i < MODEL_LEN; i++) { CHECK_EQ(tensorI8[i], (int8_t)host[ECG_DEN_WINDOW_LEN - 1]); }

    free(host);
    free(tensorF);
    free(tensorI8);
}

static void
test_narrower_model_is_clamped(void) {
    const int narrow = ECG_DEN_WINDOW_LEN / 2;
    float *host = (float *)xalloc(ECG_DEN_WINDOW_LEN * sizeof(float));
    float *tensor = (float *)xalloc(narrow * sizeof(float));

    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) { host[i] = HOST_SENTINEL; }
    for (int i = 0; i < narrow; i++) { tensor[i] = (float)i; }

    /* ecg_*_init rejects this shape, so it is defensive rather than deployed. */
    TEST_CASE("a model narrower than the host window does not over-read");
    hkv_tensor_output_f32(host, hkv_host_len(ECG_DEN_WINDOW_LEN), tensor, hkv_tensor_len(narrow), 0);
    for (int i = 0; i < narrow; i++) { CHECK_FEQ(host[i], (float)i); }
    for (int i = narrow; i < ECG_DEN_WINDOW_LEN; i++) { CHECK_FEQ(host[i], HOST_SENTINEL); }

    /* Re-fill the host buffer with a pattern the tensor does not already hold,
     * otherwise the copy back would be checked against the values the output
     * copy above just wrote and the assertion could not fail. */
    for (int i = 0; i < ECG_DEN_WINDOW_LEN; i++) { host[i] = (float)(1000 + i); }

    hkv_tensor_input_f32(tensor, hkv_tensor_len(narrow), host, hkv_host_len(ECG_DEN_WINDOW_LEN));
    for (int i = 0; i < narrow; i++) { CHECK_FEQ(tensor[i], host[i]); }

    free(host);
    free(tensor);
}

int
main(void) {
    CHECK(MODEL_LEN > ECG_DEN_WINDOW_LEN);
    CHECK(MODEL_LEN > ECG_SEG_WINDOW_LEN);

    test_denoise_output_f32();
    test_denoise_output_i8();
    test_segmentation_output_mask();
    test_segmentation_output_mask_f32();
    test_input_tail_is_defined();
    test_narrower_model_is_clamped();

    return TEST_RESULT();
}
