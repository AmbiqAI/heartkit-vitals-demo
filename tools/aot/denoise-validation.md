# Denoise AOT parity, issue #37

## Verified on AP510B

On 2026-09-08, the bare-metal parity image compared the original denoise
flatbuffer through heliaRT/TFLM against its helia-aot 0.21.0 FP32 conversion.
Both ran in the same image with shared-SRAM arenas and cold MRAM constants.
The ns-cmsis-nn dependency was v7.32.0 with FP32 enabled.

- Eight saved ECG cases, 256 FP32 output samples each, in LP and HP modes.
- All 16 denoise comparisons passed. All outputs were finite.
- Gate: per-sample absolute error <= 1e-5 + 1e-5 * abs(TFLM reference).
- Worst measured absolute error: approximately 4.291e-6 in model-output units.
  Telemetry truncates errors to integer nanounits.
- The accepted segmentation and arrhythmia modules were not regenerated;
  each passed 8/8 same-device TFLM comparisons in both modes.

This is numerical parity evidence on these fixtures, not clinical validation,
exhaustive input coverage, or USB/BLE production regression acceptance.
The production denoise adapter uses AOT. heliaRT is absent from the production
manifest/lock and ELF. The optional TFLM reference requires an external checkout.
The board was subsequently flashed with the AOT-only production demo.

## Production integration checks

All three targets compiled with Arm GNU Toolchain 15.2.rel1. Each ELF passed
`scripts/check-aot-only.sh`: all three AOT run entry points present, with no
TFLM/heliaRT interpreter symbols.

| Target | Local binary SHA-256 |
| --- | --- |
| apollo510b_evb | 9b657c48e31385b0afe7965f5d147efc56387a9c88c55f0b9de21541f8cad40c |
| apollo510_evb | a3940e3c381ab7a9e35fa871134c59a60b99af425d58eb9a126b370ed839acb5 |
| apollo330mP_evb | 8cb923e4937d0682ae75966ecfaf412d04d16cd230442d58d237c6dbcb0c7456 |

AP510B was flashed through nsx. Python USB checks with all three AI modes enabled:

| Run | USB duration | Packets | CRC errors |
| --- | --- | --- | --- |
| Initial LP sanity | 25.1 s | 731 | 0 |
| HP bench | 105.0 s | 3359 | 0 |
| LP bench | 105.0 s | 3345 | 0 |

Each bench included 90 seconds of concurrent SWO capture. Both returned ECG,
PPG, and CPU signal/metric packets, finite metrics, advancing inference counters,
and zero reported model/pipeline errors. The UIO echoes confirmed the requested
speed and all three AI modes. These are transport and pipeline sanity checks,
not equivalence of physiological outputs across different stimulus windows.

Eight host tests passed under ASan/UBSan, including denoise adapter crop and
error-path tests; four denoise-report Python tests and 132 release-helper tests
passed. Module regeneration, golden fixtures, lock checks, shell lint, and
third-party-notice regeneration checks passed. The optional external-reference
parity target also compiled.

Final TileIO real-app USB/BLE testing is assigned to the owner. AP510 and AP330
have compile coverage only, not hardware execution coverage. No release is made.

Production evidence: `/tmp/hkv-prod-usb-{lp,hp}.log` and matching `.usb`
files, `/tmp/hkv-prod-build-ap{510b,510,330}.log`, and
`/tmp/hkv-prod-tests-final.log`.

## Local generator workaround

[helia-aot #407](https://github.com/AmbiqAI/helia-aot/issues/407) tracks unprefixed
parameter globals that collide when models are linked together.
`denoise-private-params.patch` makes the 16 denoise parameter objects file-local.
`convert.sh` applies it after denoise conversion with zero fuzz. Segmentation and
arrhythmia remain pinned to 0.19.0; denoise is pinned to 0.21.0.
`bash tools/aot/convert.sh --check` passed against all three module trees.

## Evidence and reproduction

Local capture: `/tmp/hkv-den-finite-swo.log`.
Flash/build log: `/tmp/hkv-den-finite-flash.log`.
Reproducibility log: `/tmp/hkv-den-repro.log`.

Run `python3 tools/aot/denoise_report.py <capture>` for the denoise gate.
The existing `parity_report.py` gates segmentation and arrhythmia; give it one
complete repeated report, not the entire multi-report capture. The validated
single report is `/tmp/hkv-den-finite-single.log`, with report output at
`/tmp/hkv-den-seg-arr-report.md`.

The finite-value check inspects FP32 exponent bits so it remains effective
under the firmware build flags, including `-ffast-math`.
