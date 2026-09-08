#pragma once
#include <stddef.h>
#include <stdint.h>
#define hkv_denoise_input_0_size 256
#define hkv_denoise_output_0_size 256
#define hkv_denoise_arena_sram_size 65536
#define hkv_denoise_status_ok 0
struct hkv_denoise_io { int8_t *data; size_t size; };
struct hkv_denoise_model_context_t {
    hkv_denoise_io inputs[1], outputs[1];
};
int32_t hkv_denoise_model_init(hkv_denoise_model_context_t *);
int32_t hkv_denoise_model_run(hkv_denoise_model_context_t *);
