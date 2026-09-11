#pragma once

#include "hkv_denoise_common.h"

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
int32_t hkv_denoise_depthwise_conv_2d_20_init(
    hkv_denoise_model_context_t *ctx
);

/**
 * @brief Perform the operation
 *
 * @param[in] ctx  Context struct.
 *
 * @return 0 on SUCCESS
 */
int32_t hkv_denoise_depthwise_conv_2d_20_run(
    hkv_denoise_model_context_t *ctx
);

#ifdef __cplusplus
}
#endif