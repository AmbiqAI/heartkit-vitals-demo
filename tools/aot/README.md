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
preprocessed with. `den` fixtures are included for completeness; denoise stays
on TFLM.

Tolerances: segmentation 1 (int8 output, 1 LSB); arrhythmia 0.008 (float32
softmax, about 2 LSB of the int8 1/256 probability scale). Choosing a
`--test.tolerance` for the converted module is out of scope here.

### Generate

`tools/aot/make_golden.py` generates golden input/output pairs for the three ECG
models in `assets/`, so a heliaAOT-generated module can be checked against the
LiteRT reference on the same stimulus.

```sh
uv run --group aot python tools/aot/make_golden.py --model seg --out out/golden-seg.npz
uv run --group aot python tools/aot/make_golden.py --model arr --out out/golden-arr.npz
uv run --group aot python tools/aot/make_golden.py --model den --out out/golden-den.npz
```

`--model` picks `den` (`assets/denoise.tflite`), `seg`
(`assets/segmentation.tflite`) or `arr` (`assets/arrhythmia.tflite`).
`--cases` (default 8) takes evenly spaced, non-overlapping windows from
`assets/ecg_stimulus.csv`.

`--check` reloads the generated set, re-runs the LiteRT interpreter on the
stored inputs, and fails unless every output is bit-identical:

```sh
uv run --group aot python tools/aot/make_golden.py --model seg --out out/golden-seg.npz --check
```

Every interpreter runs on LiteRT's builtin reference kernels with the default
XNNPACK delegate disabled, because those are the int8 semantics helia-rt and
heliaAOT match through CMSIS-NN; XNNPACK requantizes differently and its outputs
differ by several LSB. `--delegate` opts back in for comparison only.

Feed a file to the converter with `--test.golden-data`:

```sh
uv tool run --from helia-aot==0.19.0 --python 3.12 helia-aot convert \
  --model.path assets/segmentation.tflite \
  --module.path out/segmentation --module.type nsx \
  --module.prefix hkv_segmentation --platform.name apollo510b_evb \
  --test.enabled --test.golden-data out/golden-seg.npz
```

### Format

One case per `.npz`. Keys are `input_0`, `input_1`, ... and `output_0`,
`output_1`, ..., one array per model input and output, each with the model
tensor's own shape (leading batch of 1) and native dtype: int8 outputs stay
int8, they are not dequantized. heliaAOT assigns each array straight onto the
model tensor, so a stacked batch axis would resize the emitted C stimulus array
while the module still produces one case.

Every input tensor is filled from the same case window; the ECG models are
single-input.

`--out` must end in `.npz` and is case 0, so it can be passed to
`--test.golden-data` unchanged. Cases 1..N-1 are written beside it as
`<stem>_caseNN.npz`. A `<stem>.json` sidecar records the model and stimulus
paths (relative to the repo root when they live inside it), the model sha256,
case count, the ai-edge-litert version and op resolver used, stimulus window
indices, per-tensor shapes, dtypes and quantization
parameters, the filter coefficients and window constants mirrored from the
firmware with the file each came from, and the argmax class (arrhythmia) or
per-class sample histogram (segmentation) for every case.

Inputs are built by mirroring the firmware's AI-mode pipeline in `src/main.cc`
(standardize, biquad filtfilt, denoise model, valid-span concatenation). The
script docstring states exactly what is and is not mirrored. Both commands grep
`src/constants.h` and `src/store.c` for those literals first and fail if one
has drifted; pass `--no-verify-sources` to skip that check.

The stimulus csv may be one or two columns, with or without a header row; the
last column is the sample.
