#pragma once

#include "hkv_arrhythmia_common.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Perform the operation
 *
 * @param[in] ctx  Context struct.
 *
 * @return 0 on SUCCESS
 */
int32_t hkv_arrhythmia_mul_12_run(
    hkv_arrhythmia_model_context_t *ctx
);

#ifdef __cplusplus
}
#endif