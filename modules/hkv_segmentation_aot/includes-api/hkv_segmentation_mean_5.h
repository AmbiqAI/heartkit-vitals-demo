#pragma once

#include "hkv_segmentation_common.h"

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
int32_t hkv_segmentation_mean_5_init(
    hkv_segmentation_model_context_t *ctx
);

/**
 * @brief Perform the operation
 *
 * @param[in] ctx  Context struct.
 *
 * @return 0 on SUCCESS
 */
int32_t hkv_segmentation_mean_5_run(
    hkv_segmentation_model_context_t *ctx
);

#ifdef __cplusplus
}
#endif