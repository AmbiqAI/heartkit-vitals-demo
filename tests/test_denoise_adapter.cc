#include <cassert>
#include <cstring>
#include "ecg_denoise.h"
#include "hkv_denoise_model.h"

static float modelInput[256], modelOutput[256];
static int initRc, runRc, runs;
static bool badWidth;

int nsx_printf(const char *, ...) { return 0; }
int32_t hkv_denoise_model_init(hkv_denoise_model_context_t *ctx) {
    ctx->inputs[0] = {reinterpret_cast<int8_t *>(modelInput), badWidth ? 256 : sizeof(modelInput)};
    ctx->outputs[0] = {reinterpret_cast<int8_t *>(modelOutput), sizeof(modelOutput)};
    return initRc;
}
int32_t hkv_denoise_model_run(hkv_denoise_model_context_t *) {
    ++runs;
    for (int i = 0; i < 256; ++i) { modelOutput[i] = modelInput[i] * 2; }
    return runRc;
}

int main() {
    float input[256], output[256];
    for (int i = 0; i < 256; ++i) { input[i] = static_cast<float>(i); output[i] = -99; }
    assert(ecg_denoise_inference(input, output, 25, 0) != 0);
    assert(runs == 0);
    assert(ecg_denoise_init() == 0);
    assert(ecg_denoise_arena_used() == ecg_denoise_arena_size());
    assert(ecg_denoise_inference(input, output, 25, 0) == 0);
    assert(std::memcmp(input, modelInput, sizeof(input)) == 0);
    for (int i = 0; i < 256; ++i) {
        assert(output[i] == (i >= 25 && i < 231 ? input[i] * 2 : -99));
    }
    runRc = 7;
    assert(ecg_denoise_inference(input, output, 25, 0) == 7);
    runRc = 0;
    assert(ecg_denoise_inference(nullptr, output, 25, 0) != 0);
    assert(ecg_denoise_inference(input, nullptr, 25, 0) != 0);
    assert(ecg_denoise_inference(input, output, 129, 0) != 0);
    initRc = 3;
    assert(ecg_denoise_init() == 3);
    assert(ecg_denoise_inference(input, output, 25, 0) != 0);
    initRc = 0;
    badWidth = true;
    assert(ecg_denoise_init() != 0);
    assert(ecg_denoise_inference(input, output, 25, 0) != 0);
    return 0;
}
