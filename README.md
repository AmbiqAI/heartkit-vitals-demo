# HeartKit Vitals Demo

Real-time ECG and PPG capture on an Ambiq Apollo5-family EVB, with on-device AI
running the whole time and a live browser dashboard showing both the signals and
what the silicon is doing to produce them.

HeartKit Vitals Demo is an NSX firmware application. It captures ECG and PPG
from an AS7058 sensor, runs DSP and TFLM ECG pipelines on-device, computes
heart-rate, HRV, pulse-rate, and SpO2 metrics, and streams everything to the
Tileio web dashboard over USB. Apollo510B also supports Tileio over BLE.

## What the demo shows

- Live ECG and PPG waveforms captured from a real sensor, not a recording.
- Three ECG models running on-device: denoise, segmentation (P-wave, QRS,
  T-wave), and arrhythmia classification. Segmentation bands are drawn on the
  live trace.
- Derived vitals: heart rate, HRV, pulse rate, SpO2, and PPG quality.
- Device telemetry alongside the clinical signals: CPU utilization, an estimated
  battery life, AI throughput, and AI efficiency.
- A runtime speed toggle that moves the SoC between 96 MHz low-power and 250 MHz
  high-performance operation while streaming continues.

The point of the demo is the last two items. Plenty of things can draw an ECG.
This one shows the inference cost and the power consequence next to the signal.

## Hardware required

| Board | SoC | Transport support |
| --- | --- | --- |
| `apollo510_evb` | Apollo510 | USB |
| `apollo510b_evb` | Apollo510B | USB and BLE |
| `apollo330mP_evb` | Apollo330P | USB |

`apollo510b_evb` is the default target and the one to use for a demo, because it
is the only board with the BLE radio.

You also need:

- An AS7058 sensor. Sensor transport and profile selection are configured in
  `src/constants.h` through `AS7058_BOARD_PROFILE` and `AS7058_APP_PROFILE`.
- A USB cable for the data connection. This is what the browser talks to.
- A SEGGER J-Link and the programming/debug USB connection, for flashing and SWO
  only. Not needed once the board is flashed.
- Chrome or Edge. Safari does not support WebUSB.

## Quick start A: flash the prebuilt release binary

This is the path for a demo. No toolchain, no build.

1. Download the firmware package for your board from the
   [v5.0.0 release](https://github.com/AmbiqAI/heartkit-vitals-demo/releases).
2. Connect the EVB programming/debug USB cable and turn the board on.
3. Flash it:

   ```
   TODO(verify): prebuilt flash command
   ```

4. Wait for the tool to confirm a successful download, then move the USB cable
   to the data connector.

> **TODO(verify): the v5.0.0 release does not exist yet.** As of 2026-09-01 the
> newest tag in this repository is `v4.2.0` and `gh release list` returns
> nothing. Confirm the release, its asset filenames, and the flash command after
> the release build. Until then, use Quick start B.

## Quick start B: build from source

```bash
uv sync
uv run nsx configure --app-dir . --board apollo510b_evb
uv run nsx build --app-dir . --board apollo510b_evb
uv run nsx flash --app-dir . --board apollo510b_evb
```

The built firmware is written to `build/<board>/heartkit-vitals-demo.bin`.

`pyproject.toml` requires `neuralspotx>=0.7.17`; `uv sync` handles this. See
issue #21 for the toolchain bump. Full build, flash, validation, and cleanup
steps are in `docs/developer.md`.

## Connect with Tileio

Open <https://ambiqai.github.io/tileio> in Chrome or Edge and select the
built-in dashboard **HeartKit: Vital Sign Monitoring**.

Check **Settings** first: API Mode must be **LIVE**, not **Emulate**. Emulate
shows synthetic data and is not a demo of this firmware. Reload after changing
it.

**Over USB:**

1. Move the USB cable to the board's data connector.
2. Select Device, interface `usb`, Scan.
3. Pick the Ambiq device. It enumerates as `heartkit_vitals_demo`, vendor
   `Ambiq`.
4. Select, then Connect. The dashboard should read Connected, and within about
   10 seconds the waveforms move and the numeric tiles leave `--`.

**Over BLE (`apollo510b_evb` only):**

Same flow with interface `ble` instead of `usb`. Expect a higher CPU reading
than USB; see the tiles glossary below.

**The Forget Device snag.** If the board shows as already present but Connect
fails, use **Forget Device**, then `usb` -> Scan -> select -> Connect. The Scan
button is disabled while a stale device entry is active, so Forget is the only
way out. This is the reliable recovery path and it was confirmed during USB
validation. It is a stale-session recovery, not something every normal reconnect
needs.

## What you will see

### Tiles glossary

**CPU Usage.** Percent busy, computed as `100 - idle` over a 30-second rolling
window (issue #8). It includes everything the firmware does, which means the
USB or BLE transport that exists only to feed this dashboard is counted in the
number. A deployed product that streams nothing would read lower. BLE reads
higher than USB for the same reason: measurements in issues #19 and #24 put BLE
at 37.7 percent against roughly 27 to 28 percent for USB.

**CR2032 Battery Life.** This is an **estimate, not a measurement**. It models
**MCU energy only; sensor power is deliberately excluded**, because sensor draw
depends on LED count, drive strength, and sampling duty, none of which are
properties of the MCU. The model splits time into inference, general compute,
and sleep, and bills each at its own figure: datasheet values for sleep and
per-MHz compute, bench measurements for inference (issue #18). The estimate
assumes 1485 mWh, two CR2032 cells, MCU energy only, sensor excluded. Measured
27.8 days at 96 MHz on the current build (issue #17).

What it is not: it is not a product battery specification, it is not a system
power measurement, and it is not a single-coin-cell figure. The sleep term is a
projection rather than a measurement of this build, because the demo does not
actually sleep.

**AI Throughput IPS.** Inferences per second expressed as `2e6 / duration`, the
scale the host dashboard expects (`ips_from_delta_us` in `src/main.cc`). This is
a **throughput figure, not a run rate**. It answers "how fast does this model
execute when it executes", not "how often does it execute". The models actually
run about once every 2 seconds. Do not read the tile as the model firing
hundreds of times a second.

**IPS/W.** AI efficiency: throughput divided by the modelled inference power.
There are three of these, one each for denoise, segmentation, and arrhythmia. It
inherits the estimate caveat from the battery model, because the power term is
the same modelled figure.

**Speed toggle.** Switches the SoC at runtime between 96 MHz low-power and
250 MHz high-performance operation. The battery model carries a separate set of
figures for each operating point, so the battery tile responds to the toggle.

> **Caveat: high-performance mode figures are being corrected.** Issue #25
> reports that the FreeRTOS tick and the DWT timebase do not follow the
> performance mode. In 250 MHz mode the tick runs 2.604x fast (250/96) and every
> DWT-measured duration is over-reported by the same factor. The three AI
> Throughput IPS tiles under-report by 2.604x in HP mode, the battery-life tile
> is affected, and `uptime_ms` on the HKV observability lines runs fast. **Do not
> quote any high-performance-mode number from the dashboard until #25 is fixed.**
> The 96 MHz figures are unaffected. The high-performance toggle is being
> corrected under #25; until it lands, use low-power mode for the demo.

### Expected behaviour

These are correct and should not be reported as faults.

- **The ECG trace lags real time by roughly 2.5 to 5 seconds, by design.** The
  delay comes from the AI denoise and segmentation windows. It is a deliberate
  trade of a larger fixed delay for a hard bound on jitter (issue #12). On a
  scrolling trace it is invisible, but if you tap the sensor and watch for a
  response, expect a multi-second wait.
- **A steady 10 packets per second** for the ECG and PPG signal slots, one
  second of signal per second (issue #12). A counter occasionally reading 9 or
  11 is a measurement artifact.
- **Gaps are drawn only on real data loss.** The dashboard breaks the trace
  honestly rather than drawing a smooth line across missing data. A gap after a
  genuine stall is correct behaviour. In independent validation the demo passed
  11 of 11 tests with **zero gaps** over both a 3-minute baseline and a
  10-minute endurance run. The run record is held internally in
  `USB-TEST-RESULTS/RESULTS.md`; it is not published in this repository.

## Troubleshooting

| Symptom | Do this |
| --- | --- |
| Connect fails, device already listed, Scan is greyed out | **Forget Device**, then `usb` -> Scan -> select -> Connect. |
| No device in the chooser | Check the cable is on the **data** connector, not the debug connector, and that the board is powered. |
| Tiles stay at `--` | Confirm Settings -> API Mode is **LIVE**, not Emulate, and reload. |
| Dashboard shows Connected but nothing moves | Replug the data cable, then reconnect. |
| BLE will not reconnect | Forget the device in the dashboard, then re-scan on interface `ble`. Only `apollo510b_evb` has BLE. |
| USB error or disconnect mid-demo | Replug and reconnect. Streaming resumes at the normal rate; the firmware never bursts above real time to catch up. |

## Observability

Open the board-specific SWO viewer:

```bash
uv run nsx view --app-dir . --board apollo510b_evb
```

The firmware emits machine-parseable lines in the format:

```
HKV|<uptime_ms>|<seq>|<subsystem>|<k=v>...
```

The sequence number lets you tell a dropped SWO line apart from a firmware
stall. See `src/obs.c` and issue #11 for the observability rework.

Two build flags control what is emitted (`src/constants.h`):

- `EN_APP_REPORT`, default `1`. The periodic subsystem report. This is the
  always-on telemetry and it is what you normally read.
- `EN_APP_TRACE`, default `0`. Ad hoc per-event lines. Off by default and
  compiled out entirely, including the arguments.

Counters are always compiled in regardless of the flags.

## Design record

`docs/design/streaming-pipeline.md` records the real-time streaming pipeline
rework: the measured root cause of the original ECG gaps, the decision to trade
fixed delay for bounded jitter, the no-catch-up policy, and the buffer sizing
derivation.

## Known limitations

- **Stale first data after an unclean tab close (issue #13, open).** If the
  browser tab is closed uncleanly with the USB cable left in, the device sees no
  bus event, so the first data shown when the dashboard is reopened can be
  stale. Using a clean Disconnect, or replugging the cable, avoids it.
- **High-performance-mode timebase (issue #25, open).** See the caveat above.
  Do not quote 250 MHz dashboard figures.
- **TimedSignal v2 deferred to v5.1 (issue #5, open).** Owner decision,
  2026-09-01. The demo works correctly without it; the streaming fix landed
  separately and is verified on hardware.
- **Battery and IPS/W figures are modelled, not measured.** See the tiles
  glossary.

## Documentation

- `docs/fae-runbook.md` is the one-page demo runbook for trade shows.
- `docs/developer.md` explains setup, build, flash, validation, and cleanup.
- `docs/as7058_profiles.md` explains AS7058 sensor profiles and regeneration.
- `docs/design/streaming-pipeline.md` is the streaming pipeline design record.
- `DEVELOPMENT_STATUS.md` records current hardware validation and follow-up
  engineering work.

## Repository Layout

- `boards/` contains the three NSX board definitions.
- `src/` contains application and model-pipeline sources.
- `assets/` contains model, dashboard, stimulus, and AS7058 profile inputs.
- `nsx.yml` and `nsx.lock` define the reproducible NSX dependency closure.

`modules/` and `cmake/nsx/` are generated from the lockfile. Do not commit or
edit their generated contents.
