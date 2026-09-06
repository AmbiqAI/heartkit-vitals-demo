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
by the on-device parity runner below. Each model also has a
`golden-<m>.json` sidecar carrying the model sha256, the stimulus sha256,
tensor shapes and quantization, and the firmware constants the stimulus was
preprocessed with. `den` fixtures are included for completeness; denoise stays
on TFLM.

`golden/` is committed here; `tools/aot/make_golden.py` regenerates it.

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

## On-device golden parity

`tools/aot/parity/` is a separate bare-metal image, `hkv_aot_parity`, that runs
all eight golden cases per model on the board and compares the module output
against the fixture. It is not part of the firmware: it has no scheduler, USB or
sensors, and the generated `_test_case_run()` it calls resets `DWT->CYCCNT`,
which the firmware's own latency counters use.

Boot mirrors the firmware's `main()` up to the point of inference so the cycle
counts are comparable with the TFLM latencies the firmware reports: core init,
ITM/SWO before any perf-mode switch, then `nsx_power_configure()` with the same
`nsx_power_config_t` values as `src/store.c` `nsxPwrCfg`, then a
`SystemCoreClock` fix-up equivalent to `timebase_sync_to_core_clock()` (the
firmware version needs FreeRTOS, this one does not).

The full pass runs twice: once at `NSX_POWER_PERF_LOW`, then again after
switching to `NSX_POWER_PERF_HIGH` the way `set_speed_mode()` does. Every case
and summary line carries `mode=lp|hp`, and each summary carries the `clk_hz`
read from `SystemCoreClock` after that mode's switch. Numerics are recompared in
both modes; the AOT kernels are the same code at either clock, so a mode-only
difference would be a finding.

`golden_seg_cases.c/.h` and `golden_arr_cases.c/.h` are generated and committed:

```sh
python3 tools/aot/golden_to_c.py            # regenerate
python3 tools/aot/golden_to_c.py --check    # fail on drift
```

Build, flash, capture and report:

```sh
uv run nsx configure --app-dir . --board apollo510b_evb
cmake build/apollo510b_evb -DHKV_BUILD_AOT_PARITY=ON
uv run nsx flash --app-dir . --board apollo510b_evb --target hkv_aot_parity
python3 tools/bench/swo_capture.py 30 parity.log --app-dir . --board apollo510b_evb
python3 tools/aot/parity_report.py parity.log
```

`nsx` has no `-D` passthrough, so the option is set once in the CMake cache; it
persists for later `nsx build`/`nsx flash --target hkv_aot_parity` runs.

No reset is needed around the capture. `nsx view` can only attach to this
secure-reset SoC, so a capture always starts mid-run, and forcing a run with a
J-Link Commander reset desyncs the trace and truncates it. The runner measures
once at boot and re-emits the whole report every 10 s instead, so any capture
window longer than about 15 s contains a complete pass. `parity_report.py`
reports the last pass that reached `PARITY_DONE`, and prints one summary line
per mode with `mode` as the first table column.

Pass rule: segmentation within 1 output LSB with an identical thresholded mask;
arrhythmia identical argmax with max absolute probability difference <= 0.008.
The arrhythmia lines reuse the segmentation key set, where `max_lsb` is always 0
(float output), `argmax_pct`/`valid_pct` are the single-label agreement and
`mask_eq` is the thresholded firmware label from `src/ecg_arrhythmia.cc`.

Every case is compared against two references, tagged `ref=` on each line and
reported as a separate table:

- `ref=tflm` **gates**. It runs the same flatbuffers through the TFLM
  interpreter in the same image, on the same silicon, at the same operating
  point, so a difference is attributable to the AOT compiler and nothing else.
  Built by default; `-DHKV_PARITY_TFLM=OFF` drops it, and `parity_report.py`
  then exits 1 because the gate cannot be evaluated.
- `ref=golden` is **informational**. It is the host LiteRT capture, so it also
  carries LiteRT-vs-TFLM kernel differences that this runner is not asked to
  gate.

`src/tflm.cc` is linked verbatim so the op resolver matches the firmware's.
`src/ecg_segmentation.cc` and `src/ecg_arrhythmia.cc` are not: they pull
`store.h` and `pk_ecg.h`, i.e. the FreeRTOS-scheduled app state this bare-metal
image cannot stand up. `tools/aot/parity/tflm_ref.cc` mirrors their init and
invoke instead -- same flatbuffers, same arena sizes from `constants.h`, same
`AM_SHARED_RW` placement, same quantize and output handling.

The TFLM invoke is timed with the same DWT bracket as the AOT run, so the
`HKV|parity|cycles` lines give an AOT-vs-TFLM cycle comparison per model at
both `lp` and `hp` that is free of toolchain and power-config differences.
