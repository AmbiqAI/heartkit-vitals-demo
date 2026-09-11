# AP510B LP MCU-rail power sanity check

Issue: https://github.com/AmbiqAI/heartkit-vitals-demo/issues/68

## Measured

JS110 serial004204 on the owner's nominal 1.8 V MCU rail. Mean measured voltage
was 1.8101 V. AP510B probe1160002954. Firmware confirmed LP, 96000000 Hz.
Three repetitions of each state; approximately three seconds per window.

| State | Mean current, mA | Mean power, mW | Power range across repetitions, mW |
| --- | ---: | ---: | ---: |
| Awake spin loop | 3.302 | 5.977 | 5.967-5.982 |
| Denoise | 4.955 | 8.968 | 8.935-8.985 |
| Segmentation | 4.112 | 7.443 | 7.432-7.451 |
| Arrhythmia | 4.772 | 8.638 | 8.603-8.668 |
| Light WFI sleep | 2.090 | 3.783 | 3.782-3.784 |

Model windows include input copy and loop overhead, not sensor preprocessing,
transport or the full application. Each repeat completed 190 denoise calls,
50 segmentation calls and 355 arrhythmia calls. Approximate energy per call:
142.2 uJ, 449.7 uJ and 73.4 uJ respectively. No idle baseline was subtracted.

All 15 firmware windows reported rc=0 and output_ok=1. Output checks compare
the final output of each repeated window against startup output from the same
recorded input. Startup also checks finite float outputs and compares case0
against host fixtures using the accepted host/target segmentation difference.
This is a sanity check, not a replacement for full same-device TFLM parity.

## Configuration and limitations

- `tools/bench/lp_power.c`, `hkv_lp_power` opt-in target. Three production AOT
  modules; their shared SRAM arenas and production weight placement retained.
- USB, sensor and BLE stacks are not initialized. EM9305 enable held low;
  IOM6 disabled. USB/IOM/UART peripheral shutdown requested through NSX.
- Debug printing and debug peripheral disabled for measurement windows. SWO
  attached only before the quiet interval and after acquisition ended.
- Same full memory configuration for all windows. Light sleep uses normal
  WFI and HFRC-based STIMER wake; it is not a minimum-power or datasheet Sleep1
  qualification. MCU-side peripheral/clock draw and cross-rail leakage may remain.
- The first two captures failed helper startup checks, before any test windows.
  Capture01 exposed descriptor byte/element semantics across compiler versions.
  Capture02 used an incorrect 1-LSB host-golden segmentation requirement; the
  earlier on-target parity report records a 6-LSB case0 host difference while
  matching TFLM on-device. Neither capture is power evidence.
- Capture03 gate contained isolated pulses/dropouts of about80 us. Analysis
  explicitly uses a 100 us gate-only deglitch limit. Analog samples are unchanged.
  The resulting 15 windows match the firmware's order and durations (about0.3%
  difference between nominal STIMER timing and JS time). Raw capture retained.
  These results are suitable for the requested ballpark check, not precision
  characterization of transitions below that glitch interval.

## Implication for the demo

The stored LP inference assumption is 5.5 mW. All three measured model windows
are higher, by approximately35-63%. The measured spin loop is not representative
DSP work, and the measured light sleep is not the stored 0.75 mW Sleep1 profile.
Do not substitute these baselines blindly or call them datasheet violations.
No power constants or dashboard efficiency claims were changed from this run.
The separate nominal battery correction remains 1.485 Wh to 1.35 Wh.
Normal production firmware was restored afterward, including that correction.
The post-flash SWO capture shows advancing sensor/pipeline counters, zero bus
errors and zero model errors. Browser reconnection was not tested.

## Reproduction and evidence

Worktree branch work/aot-021-denoise, base cb5c565 with local helper changes.
Test binary SHA256:
`48e5e4a650ea1a97c0ad172e17711ee3f731e4864ba6736b583312193cf5bc5e`.

Capture and logs in this directory:

- `capture03.jls`: original JS110 recording (local, ignored by git).
- `capture03.csv`: measured window averages and integrated energy.
- `startup03.log`: successful LP startup (local).
- `firmware03.log`: all invocation counts, timer ticks, return/output checks (local).
- `restored-demo.log`: production-image boot/run check after test (local).

Build: enable `HKV_BUILD_LP_POWER_TEST=ON` in the AP510B CMake cache, then
`nsx build --board apollo510b_evb --target hkv_lp_power --frozen`.

Record with the local app Python environment before flashing:

```sh
.venv/bin/python -u -m pyjoulescope_driver record --serial_number 004204 \
  --open restore --duration 110 --frequency 100000 \
  --set 's/i/lsb_src=gpi0' --set 's/extio/voltage=1.8V' \
  --signals i,v,p,0 <new-capture.jls>
```

Analyze with `tools/bench/lp_power_report.py <capture.jls> --deglitch-us 100
--csv <new-report.csv>`. CSV output refuses to overwrite an existing file.
Four synthetic report tests pass, including sample-clock offset alignment,
incomplete capture rejection, missing power sample rejection and glitch limits.
