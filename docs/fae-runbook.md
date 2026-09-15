# FAE Demo Runbook

Vital Sign Monitoring, a five-minute demonstration of on-device ECG models.

Powered by [heartKIT](https://ambiqai.github.io/heartkit/), accelerated with
[heliaAOT](https://ambiqai.github.io/helia-aot/).

## Before the demo

- Download the firmware release linked in the [setup guide](../README.md).
  Use its `apollo510b/`, `apollo510/`, or `apollo330/` folder for your board
  and follow the package flashing instructions.
- Validate the actual board and sensor before presenting. See the package
  release notes for hardware coverage and validation details.
- Connect the data USB cable. Open [TileIO](https://ambiqai.github.io/tileio/)
  in Chrome or Edge and choose **Vital Sign Monitoring** in **LIVE** API mode.
  Emulate mode is synthetic data, not firmware measurements.
- Select Device, choose USB, scan, select the board, and connect.
  BLE is available on AP510B; AP510 and AP330 use USB.
- Start in LP mode. AP330 firmware enforces LP mode. The battery tile uses
  shared budgeting assumptions, as described in the [setup guide](../README.md).
- Confirm input selection, model modes, and changing waveforms. Off or DSP
  models intentionally show `--` for AI energy. AI Throughput shows `--`
  when no valid AI model results are available.

## Demo walkthrough

1. **Input.** Explain whether the selected input is live sensor data or stored
   stimulus. Do not describe a stored waveform as live sensor data.
2. **On-device AI.** Show denoising, P/QRS/T segmentation, and arrhythmia
   classification. These demonstration outputs are not diagnostic advice.
3. **Vitals.** Show the signal-derived metrics and explain the buffering delay.
4. **Compute.** AI Throughput describes execution speed, not scheduling frequency.
   CPU Usage includes processing and demo transport work.
5. **Energy.** Denoise, Segment, and Arrhythmia Efficiency display `µJ/inf`.
   Lower means less energy per inference. The values combine execution timing
   with documented inference-power references, not a live power-meter reading.
   Comparison slides provide heliaAOT versus TFLM context.
6. **Battery.** MCU Battery Life projects a two-cell scenario, not measured runtime
   of the continuously streaming demo. Sensor supply energy is excluded.
   See [battery assumptions](battery-projection.md) for capacity, power inputs,
   and the allowance used in the calculation.

Read values from the tested build rather than quoting older releases. Keep MCU
energy, whole-system battery life, and model-reference comparisons distinct.
Product runtime requires the product's own validation.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Board absent from chooser | Board power, data cable, and data connector |
| Connected but no updates | LIVE mode and input selection, then reconnect |
| Model energy shows `--` | AI mode must complete a successful inference |
| BLE reconnect fails | Disconnect and re-scan; use AP510B |
| Persistent connection failure | Replug the data cable and reload; remove the saved device only if reconnecting still fails |
| Sensor fails | Check wiring and board revision before reflashing |

## References

- [Setup and metric definitions](../README.md)
- [Releases](https://github.com/AmbiqAI/heartkit-vitals-demo/releases)
- [AP330 validation](ap330-evb-validation.md)
- [Streaming design](design/streaming-pipeline.md)
