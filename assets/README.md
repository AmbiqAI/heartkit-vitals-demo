# Assets: provenance and licensing

Everything in this folder ships with the repository under the BSD 3-Clause
License in `LICENSE` unless stated otherwise below. Third-party licenses that
apply to the firmware binaries are reproduced in `THIRD-PARTY-NOTICES.md` at
the repository root.

## Models

The three TensorFlow Lite models were trained by Ambiq using
[HeartKit](https://github.com/AmbiqAI/heartkit) (BSD-3-Clause) with the
training configurations kept alongside them in this folder:

| Model | HeartKit configuration | HeartKit task |
| --- | --- | --- |
| `denoise.tflite` | `den-tcn-sm.json` | `hk-denoise` |
| `segmentation.tflite` | `seg-4-tcn-sm.json` | `hk-segmentation-4` |
| `arrhythmia.tflite` | `arr-4-eff-sm.json` | `hk-rhythm-4` |

The models are Ambiq-authored work product and are licensed with this
repository under the BSD 3-Clause License. HeartKit itself is separately
licensed BSD-3-Clause and is not vendored here.

TODO(verify): the configurations reference the LUDB and LSAD datasets plus
HeartKit's synthetic ECG generator. The redistribution terms of the source
datasets used for training have not been reviewed, and no dataset content is
included in this repository or in the firmware.

TODO(verify): the exact HeartKit version and training run that produced each
`.tflite` file are not recorded here.

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
  consumed by the same tool. TODO(verify): whether these two were authored at
  Ambiq or derived from an ams-OSRAM preset.

## Other assets

- `ecg_stimulus.csv` is the synthetic ECG stimulus used when no sensor is
  attached. TODO(verify): generator and source of the waveform.
- `dashboard.webp` and `overview-diagram.svg` are Ambiq-authored documentation
  images, licensed with this repository.
