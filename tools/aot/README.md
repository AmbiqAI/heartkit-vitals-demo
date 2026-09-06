# AOT golden vectors

Generates golden input/output pairs for the three ECG models in `assets/`, so a
heliaAOT-generated module can be checked against the LiteRT reference on the
same stimulus. See AmbiqAI/heartkit-vitals-demo#37.

## Run

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

Feed a file to the converter with `--test.golden-data`:

```sh
uv tool run --from helia-aot==0.19.0 --python 3.12 helia-aot convert \
  --model.path assets/segmentation.tflite \
  --module.path out/segmentation --module.type nsx \
  --module.prefix hkv_segmentation --platform.name apollo510b_evb \
  --test.enabled --test.golden-data out/golden-seg.npz
```

## Format

One case per `.npz`. Keys are `input_0`, `input_1`, ... and `output_0`,
`output_1`, ..., one array per model input and output, each with the model
tensor's own shape (leading batch of 1) and native dtype: int8 outputs stay
int8, they are not dequantized. heliaAOT assigns each array straight onto the
model tensor, so a stacked batch axis would resize the emitted C stimulus array
while the module still produces one case.

`--out` is case 0, so it can be passed to `--test.golden-data` unchanged. Cases
1..N-1 are written beside it as `<stem>_caseNN.npz`. A `<stem>.json` sidecar
records the model sha256, case count, stimulus window indices, per-tensor
shapes, dtypes and quantization parameters, and the argmax class (arrhythmia)
or per-class sample histogram (segmentation) for every case.

Inputs are built by mirroring the firmware's AI-mode pipeline in `src/main.cc`
(standardize, biquad filtfilt, denoise model, valid-span concatenation). The
script docstring states exactly what is and is not mirrored.

Choosing a `--test.tolerance` for the converted module is out of scope here.
