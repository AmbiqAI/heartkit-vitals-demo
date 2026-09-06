#pragma once

#include "hkv_arrhythmia_common.h"

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
} hkv_arrhythmia_mul_desc_t;
/**
 * @brief Per-node descriptor for the shared per-channel int8 fully-connected
 *        kernel.
 *
 * ``weight_sum_id`` / ``bias_id`` are ``-1`` when the node has no precomputed
 * kernel-sum scratch / no bias. Quant params (multiplier/shift) and the
 * weight-sum scratch are resolved from ``ctx->tensor_ptrs`` at run time.
 */
typedef struct {
    int32_t input_id;
    int32_t output_id;
    int32_t weights_id;
    int32_t bias_id;
    int32_t multiplier_id;
    int32_t shift_id;
    int32_t weight_sum_id;
    int32_t weight_sum_bytes;
    cmsis_nn_dims input_dims;
    cmsis_nn_dims filter_dims;
    cmsis_nn_dims bias_dims;
    cmsis_nn_dims output_dims;
    int32_t input_offset;
    int32_t filter_offset;
    int32_t output_offset;
    int32_t activation_min;
    int32_t activation_max;
} hkv_arrhythmia_fc_per_channel_s8_desc_t;

/**
 * @brief Shared per-channel int8 fully-connected kernel.
 *
 * Collapses the per-node ``arm_fully_connected_per_channel_s8`` call site into one shared runtime.
 *
 * @param[in,out] ctx  Model context (resolves the node's tensor pointers).
 * @param[in]     d    Per-node descriptor (rodata).
 * @return CMSIS-NN status (0 on SUCCESS).
 */
int32_t hkv_arrhythmia_kernel_fc_per_channel_s8(
    hkv_arrhythmia_model_context_t *ctx,
    const hkv_arrhythmia_fc_per_channel_s8_desc_t *d
);
/**
 * @brief Shared elementwise mul kernel (int8_t).
 *
 * Collapses the per-node ``arm_mul_s8`` call site into one shared runtime.
 *
 * @param[in,out] ctx  Model context (resolves the node's tensor pointers).
 * @param[in]     d    Per-node descriptor (rodata).
 * @return CMSIS-NN status (0 on SUCCESS).
 */
int32_t hkv_arrhythmia_kernel_mul_s8(
    hkv_arrhythmia_model_context_t *ctx,
    const hkv_arrhythmia_mul_desc_t *d
);

#ifdef __cplusplus
}
#endif