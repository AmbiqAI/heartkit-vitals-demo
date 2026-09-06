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
int32_t hkv_arrhythmia_conv_2d_29_init(
    hkv_arrhythmia_model_context_t *ctx
);

/**
 * @brief Perform the operation
 *
 * @param[in] ctx  Context struct.
 *
 * @return 0 on SUCCESS
 */
int32_t hkv_arrhythmia_conv_2d_29_run(
    hkv_arrhythmia_model_context_t *ctx
);

#ifdef __cplusplus
}
#endif