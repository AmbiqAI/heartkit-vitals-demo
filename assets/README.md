# Assets: provenance and licensing

Everything in this folder ships with the repository under the BSD 3-Clause
License in `LICENSE` unless stated otherwise below. Third-party licenses that
apply to the firmware binaries are reproduced in `THIRD-PARTY-NOTICES.md` at
the repository root.

## Models

The three TensorFlow Lite models were trained by Ambiq using
[heartKIT](https://github.com/AmbiqAI/heartkit) (BSD-3-Clause) with the
training configurations kept alongside them in this folder. heartKIT itself is
separately licensed BSD-3-Clause and is not vendored here.

Production firmware executes generated heliaAOT modules in `modules/hkv_*_aot/`,
not a TensorFlow Lite interpreter. The `.tflite` files below are the source
model assets used for conversion and reference validation. Generated modules
carry their own heliaAOT license, separate from the source models' BSD license.

| Field | `denoise.tflite` | `segmentation.tflite` | `arrhythmia.tflite` |
| --- | --- | --- | --- |
| Configuration | `den-tcn-sm.json` (task `hk-denoise`) | `seg-4-tcn-sm.json` (task `hk-segmentation-4`) | `arr-4-eff-sm.json` (task `hk-rhythm-4`, run name `arr-4-eff-sm-ei`) |
| Architecture | TCN, four blocks, 8/16/24/32 filters, kernel 1x7, dilation 1/1/2/4; FP32 weights, float32 I/O | TCN, four blocks, 16/24/32/48 filters, kernel 1x7, dilation 1/2/4/8, dropout 0.1; INT8 post-training quantization, int8 I/O | EfficientNetV2, 16 input filters and five blocks, 24/32/48/64/80 filters, kernel 1x9, stride 1x2; FP32 weights, float32 I/O |
| Input window and stride | 256 samples at 100 Hz (2.56 s), input and output `[1, 256, 1]` float32; the firmware advances 206 samples per inference (`ECG_DEN_VALID_LEN`) | 256 samples at 100 Hz (2.56 s), input `[1, 256, 1]` int8, output `[1, 256, 4]` int8 over classes NONE/P-WAVE/QRS/T-WAVE; the firmware advances 206 samples per inference (`ECG_SEG_VALID_LEN`) | 500 samples at 100 Hz (5 s), input `[1, 500, 1]` float32, output `[1, 4]` float32 over classes SR/SB/AFIB/GSVT; it runs on the first 500 samples of the 1000-sample metrics window, which advances 200 samples per cycle (`ECG_MET_VALID_LEN`) |
| Training datasets named in the configuration | `ecg-synthetic` (heartKIT's generator) at weight 0.9 and `ptbxl` at weight 0.1 | `ludb` at weight 0.20 and `ecg-synthetic` (heartKIT's generator) at weight 0.80 | `lsad` only |
| heartKIT version | heartKIT v1.0.0 (training runs were not version-tracked at the time; later models will record the exact tag or commit) | heartKIT v1.0.0 (training runs were not version-tracked at the time; later models will record the exact tag or commit) | heartKIT v1.0.0 (training runs were not version-tracked at the time; later models will record the exact tag or commit) |
| Training run | not archived | not archived | not archived |
| Dataset redistribution terms | PTB-XL on PhysioNet, license Creative Commons Attribution 4.0 International Public License, https://physionet.org/content/ptb-xl/, checked 2026-09-05. heartKIT synthetic ECG generator, BSD-3-Clause with heartKIT. | LUDB on PhysioNet, license Open Data Commons Attribution License v1.0, https://physionet.org/content/ludb/, checked 2026-09-05. heartKIT synthetic ECG generator, BSD-3-Clause with heartKIT. | LSAD (Large Scale 12-lead ECG database, Chapman-Shaoxing/Ningbo) on PhysioNet, license Creative Commons Attribution 4.0 International Public License, https://physionet.org/content/ecg-arrhythmia/, checked 2026-09-05. |
| Model license | BSD 3-Clause with this repository (`LICENSE`, `NOTICE`) | BSD 3-Clause with this repository (`LICENSE`, `NOTICE`) | BSD 3-Clause with this repository (`LICENSE`, `NOTICE`) |

The models are Ambiq-authored work product and are licensed with this
repository under the BSD 3-Clause License. No dataset content is included in
this repository or in the firmware.

SHA-256 of the shipped files, for pinning an answer to a specific artifact:

- `denoise.tflite` `e94a5788163e436d63750cb9dd462932cc281863d5d42d0c4851fd85a1b89d0a`
- `segmentation.tflite` `93c4493a1d09e1818abaffd74973d807b9e6830e5a03217b2c777abe1032e6fe`
- `arrhythmia.tflite` `a1855af0e0b2a346d773e0d36ff8c8b986c51db4aed99a6066c9da9f23f12eab`

All three files and their configurations entered the repository in a single
commit, `7e8c36d` of 2024-11-01, whose message is "feat: Latest version." The
commit itself records no heartKIT version and no training run; the values in
those two rows come from the model owner.

### Denoise frame size

The shipped `denoise.tflite` takes a 256-sample input, which is what the
firmware window `ECG_DEN_WINDOW_LEN` is sized for. `den-tcn-sm.json` now
records `frame_size` 256 so the committed configuration describes the frame of
the exported model.

## AS7058 sensor profiles

- `Life_metrics_Click_PPG-ECG.json` and `Life_metrics_Click_SpO2.json` are
  ams-OSRAM GUI configuration presets for the MIKROE Life Metrics Click board
  (they carry the ams-OSRAM `gui_as7058` schema). They are covered by the
  ams-OSRAM license terms reproduced in `THIRD-PARTY-NOTICES.md`, not by this
  repository's BSD-3-Clause license.
- `src/generated/as7058_profile_click_ppg_ecg.{c,h}` and
  `src/generated/as7058_profile_click_spo2.{c,h}` are generated from those two
  presets by `tools/as7058_json_to_profile.py`; they carry no SPDX header
  because they are generated artifacts that inherit the terms of their ams-OSRAM
  source. See `docs/as7058_profiles.md` for how to regenerate them.
- `default_click_i2c_sensor_profile.json` and
  `default_evk_spi_sensor_profile.json` are the board-default register profiles
  consumed by the same tool. Both profiles were authored at Ambiq and are
  licensed with this repository.

## Other assets

- `ecg_stimulus.csv` is the synthetic ECG stimulus used when no sensor is
  attached. Generated with the physioKIT synthetic ECG generator
  (AmbiqAI/physiokit) and licensed with physioKIT; the generator's reference
  waveforms come from open PhysioNet datasets. No PhysioNet record is included
  in this repository.
- `dashboard.webp` and `overview-diagram.svg` are Ambiq-authored documentation
  images, licensed with this repository.
