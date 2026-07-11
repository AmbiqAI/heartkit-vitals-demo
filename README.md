# HeartKit Vitals Demo

HeartKit Vitals Demo is an NSX firmware application for real-time ECG and PPG
capture with an AS7058 sensor. It runs DSP and TFLM ECG pipelines, computes
heart-rate, HRV, pulse-rate, and SpO2 metrics, and streams data to TileIO over
USB. Apollo510B also supports TileIO BLE.

## Supported Boards

| Board | SoC | Transport support |
| --- | --- | --- |
| `apollo510_evb` | Apollo510 | USB |
| `apollo510b_evb` | Apollo510B | USB and BLE |
| `apollo330mP_evb` | Apollo330P | USB |

`apollo510b_evb` is the default target. Sensor transport and profile selection
are configured in `src/constants.h` through `AS7058_BOARD_PROFILE` and
`AS7058_APP_PROFILE`.

## Quick Start

```bash
uv sync
uv run nsx configure --app-dir . --board apollo510b_evb
uv run nsx build --app-dir . --board apollo510b_evb
uv run nsx flash --app-dir . --board apollo510b_evb
```

The built firmware is written to
`build/<board>/heartkit-vitals-demo.bin`. Use `uv run nsx view --app-dir .
--board <board>` to open the board-specific SWO viewer.

## Documentation

- `docs/developer.md` explains setup, build, flash, validation, and cleanup.
- `docs/as7058_profiles.md` explains AS7058 sensor profiles and regeneration.
- `DEVELOPMENT_STATUS.md` records current hardware validation and follow-up
  engineering work.

## Repository Layout

- `boards/` contains the three NSX board definitions.
- `src/` contains application and model-pipeline sources.
- `assets/` contains model, dashboard, stimulus, and AS7058 profile inputs.
- `nsx.yml` and `nsx.lock` define the reproducible NSX dependency closure.

`modules/` and `cmake/nsx/` are generated from the lockfile. Do not commit or
edit their generated contents.
