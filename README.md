# Vital Sign Monitoring

Powered by [heartKIT](https://ambiqai.github.io/heartkit/), accelerated with
[heliaAOT](https://ambiqai.github.io/helia-aot/).

Explore on-device AI for vital sign monitoring on Ambiq evaluation boards.
View ECG and PPG waveforms, derived vitals, and AI performance together in a
live browser dashboard.

## What you can explore

- **On-device signal processing.** Three ECG models developed with heartKIT
  perform denoising, P-wave/QRS/T-wave segmentation, and arrhythmia classification.
- **Compiled AI inference.** heliaAOT turns the models into C modules that run
  directly in the firmware without an on-device model interpreter.
- **Performance and energy tradeoffs.** Compare model modes, execution speed,
  and energy per inference alongside CPU activity. Dashboard comparison slides
  show heliaAOT versus TFLM reference results.
- **Interactive evaluation.** View sensor signals or select stored stimuli,
  adjust the available controls, and see the results without rebuilding firmware.
- **A starting point for your application.** Use
  [heartKIT](https://ambiqai.github.io/heartkit/) for model development,
  [heliaAOT](https://ambiqai.github.io/helia-aot/) for compilation, and
  [neuralSPOT-X (NSX)](https://ambiqai.github.io/neuralspotx/) for the firmware
  build and deployment workflow.

The firmware processes signals on the board and sends results to
[TileIO](https://ambiqai.github.io/tileio/) over USB, or BLE on Apollo510B.

## What you need

| Evaluation board | Connection | Release package folder |
| --- | --- | --- |
| Apollo510B EVB | USB or BLE | `v520/apollo510b/` |
| Apollo510 EVB | USB | `v520/apollo510/` |
| Apollo330 Plus EVB | USB | `v520/apollo330/` |

Choose Apollo510B if you want to explore both USB and BLE. On Apollo330, use LP
mode; its battery projection is not validated.

You also need:

- A MIKROE Life Metrics Click sensor module based on the AS7058 for live ECG
  and PPG input. See the [sensor profile guide](docs/as7058_profiles.md).
- A data-capable USB cable.
- SEGGER J-Link software and access to the board's programming/debug connection
  for flashing.
- Chrome or Edge for the dashboard.

## Quick start A: flash the prebuilt release binary

No firmware toolchain is required.

1. Download `heartkit-vitals-demo-v520-firmware.zip` from the
   [v5.2.0 release](https://github.com/AmbiqAI/heartkit-vitals-demo/releases/tag/v5.2.0).
2. Unzip the package and open the folder for your board from the table above.
3. Power the board and connect its programming/debug USB port.
4. Run the helper for your computer:

   - macOS: double-click `flash_mac.command`.
   - Windows: run `flash_win.bat`.
   - Linux: run `./flash_linux.sh`.

5. Wait for `Flash completed successfully.`, then connect the board's data
   USB port to your computer.

For flashing troubleshooting, including macOS permissions and manual J-Link
commands, see `v520/FLASH.md` in the package.

## Connect with TileIO

1. Open [TileIO](https://ambiqai.github.io/tileio/) and choose the built-in
   **Vital Sign Monitoring** dashboard.
2. In Settings, select **LIVE** API mode and reload if you changed it.
3. Choose **Select Device**, select **USB**, and scan.
4. Select the device named `heartkit-vitals-demo`, then connect.

For Apollo510B over BLE, choose **BLE** instead of USB and follow the same
selection flow.

## Explore the dashboard

Choose a sensor input or a stored stimulus using **Input Select**, then enable
the AI modes you want to evaluate. Allow a few seconds for the processing
windows to fill and the display to update.

### Tiles glossary

| Tile | What it shows |
| --- | --- |
| CPU Usage | Processor activity, including signal processing and dashboard communication. |
| MCU Battery Life | Projected MCU runtime for a two-coin-cell scenario. |
| AI Throughput | Average execution speed of active AI models, in inferences per second. |
| Denoise, Segment, and Arrhythmia Efficiency | Energy per inference in `µJ/inf`. Lower is better. |
| Heart Rate, HRV, Pulse Rate, and SpO2 | Vitals derived from the selected input signals. |
| Denoise Similarity, Segmentation, and Arrhythmia Label | Signal comparison, waveform classification, and model output. |
| PPG Quality | Quality indicator for the PPG signal. |

Model efficiency tiles show `--` when the model is off, in DSP mode, or has
not produced a valid result. AI Throughput shows `--` when no AI models have
valid results.

## Troubleshooting

| Symptom | What to try |
| --- | --- |
| No device in the chooser | Check power and use a data-capable cable on the data connector, not the debug connector. |
| Saved device will not connect | Disconnect and reconnect. If it still fails, use **Forget Device** and scan again. |
| Connected but no updates | Confirm LIVE mode and the selected input, then reconnect. |
| Model efficiency shows `--` | Enable that model's AI mode and wait for a result. |
| Stale data after reopening a tab | Disconnect before closing the dashboard, or replug the data cable before reconnecting. |
| BLE will not reconnect | Re-scan using BLE on Apollo510B. |

## Quick start B: build from source

Source builds require authorized access to the private AS7058 driver dependency.
Use the prebuilt package above if you do not have access.

Follow the [developer guide](docs/developer.md) to install the prerequisites and
set up the repository, then run:

```bash
uv sync
uv run nsx configure --app-dir . --board apollo510b_evb
uv run nsx build --app-dir . --board apollo510b_evb
uv run nsx flash --app-dir . --board apollo510b_evb
```

Use `apollo510_evb` or `apollo330mP_evb` for the other boards. The firmware
binary is written to `build/<board>/heartkit-vitals-demo.bin`.

## Measurement and evaluation notes

- AI execution timing is measured. Energy per inference combines that timing
  with reference power values; it is not a live power-meter reading.
  AI Throughput describes execution speed, not how often models are scheduled.
- Battery life is a workload-based MCU projection with idle periods between
  work. It excludes sensor power and is not measured runtime of the continuously
  streaming demo. See the [calculation and measurement assumptions](docs/battery-projection.md).
- This is an evaluation demo, not a medical diagnostic device. Hardware coverage
  varies by board and interface; see the [release validation record](docs/release-v5.2.0-validation.md).

## Documentation

- [Developer guide](docs/developer.md): build, flash, diagnostics, and validation.
- [Demo walkthrough](docs/fae-runbook.md): a short guide to presenting the demo.
- [Sensor profiles](docs/as7058_profiles.md): sensor configuration and regeneration.
- [Streaming design](docs/design/streaming-pipeline.md): buffering and timing details.
- [Model provenance](assets/README.md): model inputs and licensing.
- [Contributing](CONTRIBUTING.md) and [security reporting](SECURITY.md).

## License

Ambiq-authored code in this repository is licensed under the BSD 3-Clause
License; see `LICENSE`. The models in `assets/` are covered by the same license
(provenance in `assets/README.md`).

The generated `modules/hkv_*_aot/` sources use the heliaAOT license included
in each module, which restricts use to Ambiq hardware. They are not covered
by the root BSD license. This is a mixed-license repository.

The firmware binaries also contain third-party components under their own
terms. `THIRD-PARTY-NOTICES.md` reproduces those licenses and notices; it is
generated from the modules pinned in `nsx.lock` by
`tools/release/gen_third_party_notices.py` and ships in every release package.

The AS7058 sensor driver is proprietary ams-OSRAM software. It is distributed
in binary form only, as part of the prebuilt firmware, under Ambiq's agreement
with ams-OSRAM; its source is not in this repository. See `NOTICE`.
