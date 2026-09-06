#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Generate heliaAOT golden input/output vectors for the three ECG models.

Usage:
    uv run --group aot python tools/aot/make_golden.py --model seg --out golden-seg.npz
    uv run --group aot python tools/aot/make_golden.py --model seg --out golden-seg.npz --check

Each case is written as its own `.npz`. heliaAOT's golden loader assigns the
array straight onto the model tensor (`helia_aot/aot/handlers/test_handler.py`
`tensor.data = np.array(self._golden_cache[key], copy=True)`), and the AIR
tensor setter overwrites `_shape` from the array, so a stacked batch axis would
resize the emitted C stimulus array while `model_run` still produces one case.
`tests/unit/aot/test_golden_data_required.py::_golden_arrays` builds the
accepted shape as `np.zeros(tensor.shape, dtype=tensor.dtype)`: one case, the
model's own shape (leading 1), the model's own dtype. This script matches that.

`--out` holds case 0 so it can be passed directly to
`helia-aot convert --test.golden-data`; cases 1..N-1 land beside it as
`<stem>_caseNN.npz`. A `<stem>.json` sidecar describes every case.

Firmware preprocessing mirrored here (src/main.cc EcgProcessTask, AI mode):

  raw 100 Hz sample -> pk_standardize_f32 (mean/std with NORM_STD_EPS)
      -> pk_apply_biquad_filtfilt_f32 (3-stage DF1 SOS, forward + reversed)
      -> denoise model  [den input is the output of this filter, NOT the bare
         standardized window: main.cc runs the biquad for both DenoiseModeDsp
         and DenoiseModeAi, so the AI path never sees an unfiltered window]
      -> valid span [25:231] of each 256-sample denoised window, concatenated
      -> segmentation model input (256 samples, int8 quantized)
      -> metrics stream = denoised stream from index 25 (rbEcgMet is fed
         ecgSegInout[ECG_SEG_PAD_LEN:] per segmentation window)
      -> arrhythmia model input (first 500 samples, float32)

Deliberately not mirrored:

  * nstdb_add_bw/ma/em_noise. Synthetic-mode noise injection, and appState
    defaults every level to 0 (src/store.c), so it is a no-op at boot.
  * The 200 Hz -> 100 Hz decimation in main.cc. assets/ecg_stimulus.csv is
    already the decimated stream: it equals ecg_stimulus[0::2] from
    src/stimulus.c. The firmware's decimator seeks one sample then takes one,
    i.e. it keeps the odd phase ecg_stimulus[1::2]; the csv is the even phase.
    A 5 ms phase offset does not change what the models are exercised with.

The DSP is computed in float32 but not bit-matched against CMSIS-DSP, whose
blocked accumulation differs. That only affects how representative the stimulus
is; the golden pair itself is self-consistent because `output_N` is whatever
LiteRT produced for the `input_N` stored alongside it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]

# src/constants.h
ECG_TARGET_RATE = 100
NORM_STD_EPS = np.float32(0.001)
ECG_DEN_WINDOW_LEN = 256
ECG_DEN_PAD_LEN = 25
ECG_DEN_VALID_LEN = ECG_DEN_WINDOW_LEN - 2 * ECG_DEN_PAD_LEN
ECG_SEG_WINDOW_LEN = 256
ECG_SEG_PAD_LEN = 25
ECG_ARR_WINDOW_LEN = 500
ECG_ARR_THRESHOLD = 0.4

# src/store.c: generate_arm_biquad_sos(0.5, 30, 100, order=3), {b0,b1,b2,a1,a2}
# per stage with the CMSIS sign convention (a coefficients already negated).
ECG_SOS = np.array(
    [
        [0.2467691808982006, 0.4935383617964012, 0.2467691808982006, -0.4141296048598937, -0.36229096617676754],
        [1.0, 0.0, -1.0, 0.8213745394235588, 0.14232107570294283],
        [1.0, -2.0, 1.0, 1.9684516644108876, -0.9694342914476478],
    ],
    dtype=np.float32,
)

SEG_CLASS_NAMES = ["NONE", "P-WAVE", "QRS", "T-WAVE"]
ARR_CLASS_NAMES = ["SR", "SB", "AFIB", "GSVT"]

MODELS = {
    "den": {"tflite": "assets/denoise.tflite", "config": "assets/den-tcn-sm.json"},
    "seg": {"tflite": "assets/segmentation.tflite", "config": "assets/seg-4-tcn-sm.json"},
    "arr": {"tflite": "assets/arrhythmia.tflite", "config": "assets/arr-4-eff-sm.json"},
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_stimulus(path: Path) -> np.ndarray:
    rows = path.read_text().strip().splitlines()
    if rows and rows[0].lower().startswith("index"):
        rows = rows[1:]
    return np.array([np.float32(line.split(",")[1]) for line in rows], dtype=np.float32)


def standardize(x: np.ndarray) -> np.ndarray:
    """pk_standardize_f32: (x - mean) / (std + eps), arm_std_f32 is ddof=1."""
    mu = np.float32(x.mean(dtype=np.float32))
    std = np.float32(x.std(ddof=1, dtype=np.float32)) + NORM_STD_EPS
    return ((x - mu) / std).astype(np.float32)


def _biquad_df1(x: np.ndarray) -> np.ndarray:
    """arm_biquad_cascade_df1_f32 with zeroed state, cascaded over ECG_SOS."""
    y = x.astype(np.float32)
    for b0, b1, b2, a1, a2 in ECG_SOS:
        out = np.empty_like(y)
        x1 = x2 = y1 = y2 = np.float32(0.0)
        for n in range(y.shape[0]):
            xn = y[n]
            yn = np.float32(b0 * xn + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2)
            out[n] = yn
            x2, x1 = x1, xn
            y2, y1 = y1, yn
        y = out
    return y


def filtfilt(x: np.ndarray) -> np.ndarray:
    """pk_apply_biquad_filtfilt_f32: forward pass, reverse, second pass, reverse."""
    forward = _biquad_df1(x)
    backward = _biquad_df1(forward[::-1].copy())
    return backward[::-1].copy()


def denoise_model_input(raw_window: np.ndarray) -> np.ndarray:
    return filtfilt(standardize(raw_window))


class TFLiteModel:
    """Single-input/single-output LiteRT wrapper over an on-disk .tflite."""

    def __init__(self, path: Path):
        from ai_edge_litert.interpreter import Interpreter

        self.path = path
        self.interpreter = Interpreter(model_path=str(path))
        self.interpreter.allocate_tensors()
        self.inputs = self.interpreter.get_input_details()
        self.outputs = self.interpreter.get_output_details()

    def run(self, arrays: list[np.ndarray]) -> list[np.ndarray]:
        for detail, array in zip(self.inputs, arrays, strict=True):
            self.interpreter.set_tensor(detail["index"], array)
        self.interpreter.invoke()
        return [np.array(self.interpreter.get_tensor(d["index"]), copy=True) for d in self.outputs]

    def quantize(self, detail: dict, values: np.ndarray) -> tuple[np.ndarray, int]:
        """Mirror hkv_tensor_input_i8 / hkv_tensor_input_f32 from ecg_tensor_copy.h.

        The firmware writes `(int8_t)(host[i] / scale + zeroPoint)`: a C cast,
        so truncation toward zero rather than round-to-nearest. Out-of-range
        conversion is undefined in C, so clip and report the count instead of
        guessing what the target's VCVT does.
        """
        shape = tuple(int(d) for d in detail["shape"])
        dtype = detail["dtype"]
        if dtype == np.float32:
            return values.astype(np.float32).reshape(shape), 0
        scale, zero_point = detail["quantization"]
        if scale == 0.0:
            raise ValueError(f"{self.path.name}: non-float input without affine quantization")
        raw = np.trunc(values.astype(np.float32) / np.float32(scale) + np.float32(zero_point))
        info = np.iinfo(dtype)
        clipped = int(np.count_nonzero((raw < info.min) | (raw > info.max)))
        return np.clip(raw, info.min, info.max).astype(dtype).reshape(shape), clipped


def denoised_stream(raw: np.ndarray, start: int, num_samples: int, den: TFLiteModel) -> np.ndarray:
    """Concatenate denoise valid spans the way main.cc feeds rbEcgSeg."""
    chunks = []
    produced = 0
    offset = start
    while produced < num_samples:
        window = raw[offset : offset + ECG_DEN_WINDOW_LEN]
        if window.shape[0] < ECG_DEN_WINDOW_LEN:
            raise ValueError("stimulus exhausted while building the denoised stream")
        model_input, _ = den.quantize(den.inputs[0], denoise_model_input(window))
        out = den.run([model_input])[0]
        detail = den.outputs[0]
        if detail["dtype"] != np.float32:
            scale, zero_point = detail["quantization"]
            out = (out.astype(np.float32) - np.float32(zero_point)) * np.float32(scale)
        flat = out.reshape(-1).astype(np.float32)
        chunks.append(flat[ECG_DEN_PAD_LEN : ECG_DEN_PAD_LEN + ECG_DEN_VALID_LEN])
        produced += ECG_DEN_VALID_LEN
        offset += ECG_DEN_VALID_LEN
    return np.concatenate(chunks)[:num_samples]


def stream_samples_needed(model: str) -> int:
    if model == "seg":
        return ECG_SEG_WINDOW_LEN
    return ECG_SEG_PAD_LEN + ECG_ARR_WINDOW_LEN


def raw_span(model: str) -> int:
    """Raw stimulus samples one case consumes, so cases stay non-overlapping."""
    if model == "den":
        return ECG_DEN_WINDOW_LEN
    windows = math.ceil(stream_samples_needed(model) / ECG_DEN_VALID_LEN)
    return ECG_DEN_WINDOW_LEN + (windows - 1) * ECG_DEN_VALID_LEN


def build_case_input(model: str, raw: np.ndarray, start: int, den: TFLiteModel | None) -> np.ndarray:
    """Host-side float values a case feeds the model, before quantization."""
    if model == "den":
        return denoise_model_input(raw[start : start + ECG_DEN_WINDOW_LEN])
    stream = denoised_stream(raw, start, stream_samples_needed(model), den)
    if model == "seg":
        return stream[:ECG_SEG_WINDOW_LEN]
    return stream[ECG_SEG_PAD_LEN : ECG_SEG_PAD_LEN + ECG_ARR_WINDOW_LEN]


def case_paths(out: Path, num_cases: int) -> list[Path]:
    return [out] + [out.with_name(f"{out.stem}_case{i:02d}.npz") for i in range(1, num_cases)]


def sidecar_path(out: Path) -> Path:
    return out.with_suffix(".json")


def dequantize(detail: dict, array: np.ndarray) -> np.ndarray:
    if detail["dtype"] == np.float32:
        return array.astype(np.float32)
    scale, zero_point = detail["quantization"]
    return (array.astype(np.float32) - np.float32(zero_point)) * np.float32(scale)


def tensor_meta(detail: dict) -> dict:
    scale, zero_point = detail["quantization"]
    return {
        "name": detail["name"],
        "shape": [int(d) for d in detail["shape"]],
        "dtype": np.dtype(detail["dtype"]).name,
        "quantization": {"scale": float(scale), "zero_point": int(zero_point)},
    }


def classification_summary(model: str, outputs: list[np.ndarray], details: list[dict]) -> dict:
    scores = dequantize(details[0], outputs[0])
    if model == "arr":
        flat = scores.reshape(-1)
        idx = int(np.argmax(flat))
        # ecg_arrhythmia_inference: 0 is inconclusive, classes shift up by one.
        label = idx + 1 if float(flat[idx]) > ECG_ARR_THRESHOLD else 0
        return {
            "argmax_class": idx,
            "argmax_name": ARR_CLASS_NAMES[idx],
            "argmax_score": float(flat[idx]),
            "firmware_label": label,
        }
    if model == "seg":
        # Per-sample classification: the scalar analogue of an argmax class is
        # the histogram over the window, plus the modal class.
        per_sample = np.argmax(scores.reshape(-1, scores.shape[-1]), axis=-1)
        counts = np.bincount(per_sample, minlength=len(SEG_CLASS_NAMES))
        return {
            "argmax_class_counts": {SEG_CLASS_NAMES[i]: int(counts[i]) for i in range(len(SEG_CLASS_NAMES))},
            "argmax_class_modal": int(np.argmax(counts)),
            "argmax_name_modal": SEG_CLASS_NAMES[int(np.argmax(counts))],
        }
    return {}


def generate(args: argparse.Namespace) -> int:
    spec = MODELS[args.model]
    tflite_path = (REPO_ROOT / spec["tflite"]).resolve()
    stimulus_path = Path(args.stimulus).resolve()
    out = Path(args.out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)

    raw = load_stimulus(stimulus_path)
    span = raw_span(args.model)
    stride = raw.shape[0] // args.cases
    if stride < span:
        print(
            f"error: {args.cases} evenly spaced cases give a stride of {stride} raw samples "
            f"but a {args.model} case consumes {span}; reduce --cases",
            file=sys.stderr,
        )
        return 2
    starts = [k * stride for k in range(args.cases)]

    model = TFLiteModel(tflite_path)
    den = model if args.model == "den" else TFLiteModel((REPO_ROOT / MODELS["den"]["tflite"]).resolve())

    cases = []
    for case_index, (start, path) in enumerate(zip(starts, case_paths(out, args.cases), strict=True)):
        values = build_case_input(args.model, raw, start, den)
        model_input, clipped = model.quantize(model.inputs[0], values)
        outputs = model.run([model_input])
        arrays = {"input_0": model_input}
        for idx, array in enumerate(outputs):
            arrays[f"output_{idx}"] = array
        np.savez(path, **arrays)
        record = {
            "case": case_index,
            "path": path.name,
            "stimulus_start": int(start),
            "stimulus_end": int(start + span),
            "input_clipped_samples": clipped,
        }
        record.update(classification_summary(args.model, outputs, model.outputs))
        cases.append(record)
        print(f"[{args.model}] case {case_index}: raw[{start}:{start + span}] -> {path}")

    meta = {
        "model": args.model,
        "model_path": str(tflite_path.relative_to(REPO_ROOT)),
        "model_sha256": sha256_file(tflite_path),
        "stimulus_path": str(stimulus_path),
        "stimulus_sha256": sha256_file(stimulus_path),
        "sample_rate_hz": ECG_TARGET_RATE,
        "num_cases": args.cases,
        "stimulus_stride": int(stride),
        "raw_samples_per_case": span,
        "layout": "one case per npz, keys input_N/output_N, model tensor shape and dtype",
        "tensors": {
            **{f"input_{i}": tensor_meta(d) for i, d in enumerate(model.inputs)},
            **{f"output_{i}": tensor_meta(d) for i, d in enumerate(model.outputs)},
        },
        "cases": cases,
    }
    sidecar_path(out).write_text(json.dumps(meta, indent=2) + "\n")
    print(f"[{args.model}] sidecar -> {sidecar_path(out)}")
    return 0


def check(args: argparse.Namespace) -> int:
    out = Path(args.out).resolve()
    sidecar = sidecar_path(out)
    if not sidecar.exists():
        print(f"error: sidecar {sidecar} not found; generate before --check", file=sys.stderr)
        return 2
    meta = json.loads(sidecar.read_text())
    tflite_path = (REPO_ROOT / meta["model_path"]).resolve()
    if sha256_file(tflite_path) != meta["model_sha256"]:
        print(f"error: {tflite_path} no longer matches the sha256 recorded in the sidecar", file=sys.stderr)
        return 1

    model = TFLiteModel(tflite_path)
    failures = 0
    for record in meta["cases"]:
        path = out.with_name(record["path"])
        loaded = np.load(path, allow_pickle=False)
        arrays = [loaded[f"input_{i}"] for i in range(len(model.inputs))]
        outputs = model.run(arrays)
        for idx, actual in enumerate(outputs):
            expected = loaded[f"output_{idx}"]
            if actual.dtype != expected.dtype or actual.shape != expected.shape:
                print(f"FAIL {path.name} output_{idx}: {expected.dtype}{expected.shape} != {actual.dtype}{actual.shape}")
                failures += 1
            elif not np.array_equal(actual, expected):
                worst = float(np.max(np.abs(actual.astype(np.float64) - expected.astype(np.float64))))
                print(f"FAIL {path.name} output_{idx}: not bit-identical, max abs diff {worst}")
                failures += 1
            else:
                print(f"ok   {path.name} output_{idx}: bit-identical {expected.dtype}{expected.shape}")
    if failures:
        print(f"--check failed: {failures} mismatch(es)", file=sys.stderr)
        return 1
    print(f"--check passed: {len(meta['cases'])} case(s) reproduce bit-identically")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--model", required=True, choices=sorted(MODELS))
    parser.add_argument("--out", required=True, help="npz path for case 0; later cases land beside it")
    parser.add_argument("--cases", type=int, default=8, help="number of evenly spaced, non-overlapping cases")
    parser.add_argument(
        "--stimulus",
        default=str(REPO_ROOT / "assets" / "ecg_stimulus.csv"),
        help="stimulus csv (index,value) at the pipeline's 100 Hz rate",
    )
    parser.add_argument("--check", action="store_true", help="reload the npz set and re-run the interpreter")
    args = parser.parse_args()
    if args.cases < 1:
        parser.error("--cases must be at least 1")
    return check(args) if args.check else generate(args)


if __name__ == "__main__":
    sys.exit(main())
