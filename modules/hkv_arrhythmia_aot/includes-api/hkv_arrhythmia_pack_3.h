#pragma once

#include "hkv_arrhythmia_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the operator
 *
 * @param[in] ctx  Context struct.
 *
 * @return 0 on SUCCESS
 */
int32_t hkv_arrhythmia_pack_3_init(
    hkv_arrhythmia_model_context_t *ctx
);

/**
 * @brief Perform the operation
 *
 * @param[in] ctx  Context struct.
 * @param input_0  Pointer to the input0 buffer.
 * @param input_1  Pointer to the input1 buffer.
 * @param input_2  Pointer to the input2 buffer.
 * @param input_3  Pointer to the input3 buffer.
 * @param output  Pointer to the output buffer.
 *
 * @return 0 on SUCCESS
 */
int32_t hkv_arrhythmia_pack_3_run(
    hkv_arrhythmia_model_context_t *ctx
);

#ifdef __cplusplus
}
#endif