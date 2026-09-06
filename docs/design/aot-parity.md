# Design record: helia-aot numeric parity for segmentation and arrhythmia

Status: proposed, pending owner approval (2026-09-06)
Scope: AOT-compiled ECG segmentation and arrhythmia modules, issue #37
Inputs: host golden generation, on-device parity runs, and a per-op dump bisect
against a LiteRT all-tensor reference. Tooling is on branches `37-aot-golden`
(`tools/aot/make_golden.py`) and `37-aot-parity` (`tools/aot/parity/`), not `main`.

## 1. Purpose

Define what "the AOT module is correct" means before AOT replaces the
interpreter in firmware. The reference of record is **TFLM on device**, not
LiteRT on host: section 4 shows the LiteRT delta already ships, so gating on
LiteRT would gate on a difference the product does not have.

## 2. Method

Golden vectors, host side (`tools/aot/make_golden.py`):

- 8 windows of `assets/ecg_stimulus.csv` through the firmware preprocessing
  chain, so the input is the firmware's and not a notebook's.
- LiteRT **builtin kernels with delegates off**. One `.npz` per case plus a sidecar
  JSON carrying provenance and a firmware-literal drift check.

On-device runner (`tools/aot/parity/main.c`, target `hkv_aot_parity`):

- Bare-metal nsx target, firmware power configuration applied, LP and HP passes.
- Emits `HKV|parity|...` over SWO; `tools/aot/parity_report.py` parses.
- `HKV_PARITY_DUMP_OPS` dumps every operator output for segmentation case 0,
  compared on host against a LiteRT all-tensor dump.

Reproduction artifacts (scratchpad): `parity-final.log`, `parity-run2.log`
(the 5.8 M cycle figure in section 6), `parity-run3.log` (the gate run of
section 5), `opdump-seg-case0.log`, `compare_ops.py`, `sim_ops.py`,
`sim_conv_rounding.py`, `analyze_mean.py`, `aot-eval/golden-*.npz`,
`aot-eval/golden-arr.xnnpack.npz`, `aot-eval/diag/seg_case0_ref.{npz,json}`.

## 3. Findings, segmentation case 0 (31 ops)

Per-op numbers are from `opdump-seg-case0.log`; the whole-model figures below
the table are from `parity-final.log`.

| Op class | Result | Cause |
|---|---|---|
| RESHAPE, MINIMUM, RELU, MUL | exact | |
| CONV 1x1 (op 3) | 1 of 4096 elements off by 1 LSB | LiteRT's 1x1 path rounds once; CMSIS-NN requantize double-rounds |
| DEPTHWISE_CONV | exact | LiteRT double-rounds here too |
| MEAN | 6 of 16 channels differ | LiteRT requantizes the raw sum then adds a folded bias; `arm_mean_s8` subtracts `input_zp * N` first |
| SOFTMAX output | 6 to 12 LSB over the 8 cases | 1 LSB residuals compounded by three squeeze-and-excite MUL gates |

The op-3 element is index 594: accumulator 6631, multiplier 1834215273, shift -6,
true fraction 0.4952, exactly where single and double rounding separate. MEAN is a
codegen-form difference, not a parameter bug: the multiplier and shift are
bit-identical to `QuantizeMultiplier(in_scale / (N * out_scale))`.

Whole-model effect (`parity-final.log`): argmax agreement 98.05 to 100 percent and
the thresholded mask differs on all 8 cases. Arrhythmia argmax and firmware label
agree on all 8 cases; two cases exceed the 0.008 probability band.

## 4. Why LiteRT is not the bar

The shipped TFLM firmware (`nsx-helia-rt` 1.16.0, backend `helia`, `nsx.lock:279`)
calls the same ns-cmsis-nn kernels for every op in these two models:
`arm_mean_s8`, `arm_convolve_wrapper_s8`, `arm_depthwise_conv_wrapper_s8`,
`arm_mul_s8`, `arm_minimum_s8`, `arm_relu_s8`, `arm_softmax_s8`
(`modules/helia-rt/tensorflow/lite/micro/kernels/helia/*.cc`). **The LiteRT delta
ships today.** LiteRT is also not one fixed truth: XNNPACK, its default delegate,
differs from its own builtin kernels by up to 13 LSB here. Hence delegates off.

## 5. Gate definition

**Primary (blocking), AOT versus TFLM on device, same 8 cases:** segmentation
within 1 LSB with an identical thresholded mask; arrhythmia identical argmax and
firmware label, probabilities within 0.008. Status: **met. Measured 8/8 on both
models at both operating points (`parity-run3.log`, `HKV_PARITY_TFLM=ON`):
segmentation is bit-exact against TFLM, max 0 LSB with an identical mask on
every case; arrhythmia agrees on argmax and firmware label with a maximum
probability difference of 0.0078, two int8 LSB of the 1/256 output scale.**

That result is the direct confirmation of section 4: the whole LiteRT delta in
section 3 is a LiteRT-versus-TFLM kernel difference that the product already
ships, and none of it is attributable to the AOT compiler.

**Secondary (informational):** AOT versus the LiteRT builtin reference, max LSB
and argmax agreement per case. Not a pass/fail gate. Measured: segmentation 6 to
12 LSB with the mask differing on all 8 cases; arrhythmia 6/8 within the band,
cases 3 and 5 at 0.0195 and 0.0117.

Both references are emitted by the same runner, tagged `ref=` per line, and
`tools/aot/parity_report.py` exits non-zero on the primary gate only.

## 6. Timing and layout

DWT cycles are comparable only when the runner applies the firmware power
configuration. Without it segmentation read 13.7 M cycles against 5.8 M with it
(`parity-run2.log`): a reporting error, not a regression.

Mean cycles per run, both runtimes in the same image at the same operating point
(`parity-run3.log`):

| mode | clk_hz | model | AOT | TFLM |
|---|---|---|---|---|
| lp | 96 M | seg | 5,787,671 | 5,807,081 |
| lp | 96 M | arr | 813,204 | 849,687 |
| hp | 250 M | seg | 5,795,163 | 5,815,663 |
| hp | 250 M | arr | 828,119 | 872,268 |

Cycle counts, not wall time: at this layout AOT is within about 0.3 percent of
TFLM on segmentation and about 4 percent faster on arrhythmia. The case for AOT
here is not the interpreter overhead, which is already small next to the kernel
work. Mirror-layout rule for the first comparison:
constants cold in MRAM, scratch in shared SRAM, matching firmware. TCM placement
and multi-arena are later measured optimizations, not part of the parity run.

## 7. Open items

- Whether AOT MEAN codegen should adopt the LiteRT bias-after-requantize form
  (upstream helia-aot question, not a demo-side fix).
- Ops 13/21 and 12/20 were not individually re-simulated.
- Two arrhythmia cases sit at 0.0117 and 0.0195 against LiteRT, above the 0.008
  band. Confirmed as cases 5 and 3 in `parity_report.py` ordering; both are
  inside the band against TFLM, so they are LiteRT delta, not AOT error.
- The generated `hkv_segmentation_test_case_run()` self-check returns rc=1 while
  all 8 golden cases pass against TFLM. The generator's own bundled vector is
  the suspect, not the module; not chased here.
- The generated context reports its `size` field as bytes while the value is in
  elements. A helia-aot documentation or field-naming fix, not a demo-side one.
- Filed upstream from this work: AmbiqAI/helia-aot#388 and
  AmbiqAI/ns-cmsis-nn#468 (the MEAN and requantize rounding forms above),
  AmbiqAI/helia-aot#389 (float MUL broadcast), AmbiqAI/helia-aot#390 (the I/O
  `size` field units above), and AmbiqAI/helia-aot#391 (dynamic tensor
  warning).
