# heliaAOT model modules

`modules/hkv_segmentation_aot/` and `modules/hkv_arrhythmia_aot/` are generated
C, committed to the repo. They are declared in `nsx.yml` as `source: {vendored:
true}`, so `nsx sync` never touches them and `nsx.lock` pins the content hash of
each directory.

The modules compile as part of the firmware build but nothing links them yet;
the segmentation and arrhythmia adapters still call TFLM. See
AmbiqAI/heartkit-vitals-demo#37.

## Memory layout

The first AOT build mirrors what TFLM does today: weights cold in MRAM as XIP
`.rodata`, working arena in shared SRAM. The YAML rules place the `constant`
tensors in MRAM and the `scratch` tensors in SRAM; the generator then emits
`<prefix>_arena_sram_buffer` guarded by `<PREFIX>_PUT_IN_SRAM`, which defaults
to a no-op. `hkv_aot_attributes.h` defines those macros as
`__attribute__((section(".shared")))`, the same section `AM_SHARED_RW` puts the
TFLM arenas in, and the app `CMakeLists.txt` force-includes it through each
module's `<MODULE>_ATTRIBUTES_HEADER` variable before the modules are added.
Neither model has `persistent` tensors, so no rule is needed for that kind.
Moving the arenas to TCM is a later optimization.

## Regenerate

```sh
tools/aot/convert.sh
```

Converts `assets/segmentation.tflite` and `assets/arrhythmia.tflite` with
helia-aot 0.19.0 using `segmentation.yaml` / `arrhythmia.yaml`, then re-runs
`nsx lock`. Commit both the module trees and `nsx.lock`.

## Check

```sh
tools/aot/convert.sh --check
```

Regenerates into a temp directory and diffs against the committed trees,
ignoring the generation timestamp in the file banners. Non-zero exit means the
committed modules no longer match the converter and the YAML.

`nsx sync --frozen` and `nsx lock --check` (both in `scripts/ci-local.sh
frozen`) catch hand edits to the committed trees.

## Golden data

`golden/` holds the reference input/output pairs used by the generated
`--test.enabled` case: eight cases per model. Only `golden-<m>.npz` (case 0)
feeds the generated on-device test case; `golden-<m>_caseNN.npz` are consumed
by the host parity runner (branch 37-aot-parity). Each model also has a
`golden-<m>.json` sidecar carrying the model sha256, the stimulus sha256,
tensor shapes and quantization, and the firmware constants the stimulus was
preprocessed with. `den` fixtures are
included for completeness; denoise stays on TFLM.

The generator, `tools/aot/make_golden.py`, lands with PR #77 — until then,
regenerate goldens from that branch.

Tolerances: segmentation 1 (int8 output, 1 LSB); arrhythmia 0.008 (float32
softmax, about 2 LSB of the int8 1/256 probability scale).
