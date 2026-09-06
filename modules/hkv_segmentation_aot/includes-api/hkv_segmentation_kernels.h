#pragma once

#include "hkv_segmentation_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-node descriptor for the shared elementwise-multiply kernel.
 */
typedef struct {
    int32_t input1_id;
    int32_t input2_id;
    int32_t output_id;
    cmsis_nn_dims input1_dims;
    cmsis_nn_dims input2_dims;
    cmsis_nn_dims output_dims;
    int32_t input1_offset;
    int32_t input2_offset;
    int32_t output_offset;
    int32_t output_multiplier;
    int32_t output_shift;
    int32_t activation_min;
    int32_t activation_max;
} hkv_segmentation_mul_desc_t;

/**
 * @brief Shared elementwise mul kernel (int8_t).
 *
 * Collapses the per-node ``arm_mul_s8`` call site into one shared runtime.
 *
 * @param[in,out] ctx  Model context (resolves the node's tensor pointers).
 * @param[in]     d    Per-node descriptor (rodata).
 * @return CMSIS-NN status (0 on SUCCESS).
 */
int32_t hkv_segmentation_kernel_mul_s8(
    hkv_segmentation_model_context_t *ctx,
    const hkv_segmentation_mul_desc_t *d
);

#ifdef __cplusplus
}
#endif