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
