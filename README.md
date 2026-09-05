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
- Device telemetry alongside the clinical signals: CPU utilization, a modelled
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

Two ways to get firmware onto the board. Build from source works today. The
prebuilt package becomes available with the v5.0.0 release.

## Quick start B: build from source

```bash
uv sync
uv run nsx configure --app-dir . --board apollo510b_evb
uv run nsx build --app-dir . --board apollo510b_evb
uv run nsx flash --app-dir . --board apollo510b_evb
```

The built firmware is written to `build/<board>/heartkit-vitals-demo.bin`.

`pyproject.toml` requires `neuralspotx>=0.7.17`; `uv sync` handles this. Full
build, flash, validation, and cleanup steps are in `docs/developer.md`.

Building from source requires access to the private `nsx-as7058` module pinned
in `nsx.lock`; without it `nsx configure` cannot fetch the AS7058 driver. For
everyone else the prebuilt package in Quick start A is the supported path.

## Quick start A: flash the prebuilt release binary

No toolchain required. You need the SEGGER J-Link software installed and the
EVB connected on its programming/debug USB port, powered on.

1. Download `heartkit-vitals-demo-v500-firmware.zip` from the
   [v5.0.0 release](https://github.com/AmbiqAI/heartkit-vitals-demo/releases/tag/v5.0.0),
   or from the team OneDrive under
   `Demos/vital-sign-monitoring/firmware/v500/`.
2. Unzip it, pick your board folder from the table below, and open it:

   | Your EVB | Folder | Transports |
   | --- | --- | --- |
   | Apollo510B EVB | `v500/apollo510b/` | USB and BLE |
   | Apollo510 EVB | `v500/apollo510/` | USB only |
   | Apollo330 Plus EVB | `v500/apollo330/` | USB only |

   The Apollo510 and Apollo330 packages are USB only; BLE is available on the
   Apollo510B alone. On the Apollo330, keep the speed toggle in low-power mode
   only and do not quote the battery tile. Both points are covered under Board
   differences below.
3. Run the helper for your computer: double-click `flash_mac.command` on macOS,
   run `flash_win.bat` on Windows, or run `./flash_linux.sh` on Linux.
4. Wait for `Flash completed successfully.`, then move the USB cable to the data
   connector.

<!-- Maintainers: these J-Link values are resolved from the SoC facts file
     (modules/nsx-ambiq-sdk/cmake/socs/facts/apollo510b.cmake) plus any board
     override in boards/<board>/debug.cmake. They are duplicated here for
     reader convenience only. If they change, update this line and
     tools/release/package.sh together. -->

If the helper does not run, flash from inside that same folder with
`JLinkExe -nogui 1 -device AP510NFA-CBR -if SWD -speed 4000 -commandfile downloadfw.jlink`.
The `apollo510b` and `apollo510` folders both use device `AP510NFA-CBR`; in the
`apollo330` folder use `-device Apollo330P_510L` instead.

To confirm, check that the board enumerates as `heartkit-vitals-demo` on the
WebUSB port. Full instructions, including the macOS Gatekeeper workaround, are
in `v500/FLASH.md`.

## Connect with Tileio

Open <https://ambiqai.github.io/tileio> in Chrome or Edge and select the
built-in dashboard **HeartKit: Vital Sign Monitoring**.

Check **Settings** first: API Mode must be **LIVE**, not **Emulate**. Emulate
shows synthetic data and is not a demo of this firmware. Reload after changing
it.

**Over USB:**

1. Move the USB cable to the board's data connector.
2. Select Device, interface `usb`, Scan.
3. Pick the Ambiq device. It enumerates with the product string
   `heartkit-vitals-demo` (`src/main.cc`), vendor `Ambiq`.
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

### Board differences

The same firmware sources build for all three boards. The streaming pipeline,
the sensor path, and the dashboard behave the same on every board. Only the
points below differ.

- **Transports.** `apollo510b_evb` supports USB and BLE. `apollo510_evb` and
  `apollo330mP_evb` are USB only.
- **Apollo510 figures are sourced.** The battery and clock figures used on the
  Apollo510 come from its own datasheet, Apollo510 SoC Datasheet DS-A510-1p1p0
  Table 39 p.250. The values are identical to the Apollo510B ones. The v5.0.0
  timebase fix is active on this board and both speed modes work as they do on
  the 510B.
- **Apollo330: low-power mode only.** High-performance mode is not supported on
  this board in v5.0.0. The timebase sync is a no-op there, so the toggle
  selects 192 MHz and the timing-derived tiles misreport. Keep the speed toggle
  in low-power mode.
- **Apollo330: the battery tile is not sourced.** On this board the battery tile
  uses unsourced fallback figures. Do not quote it. Sourced Apollo330 Plus
  figures, from the preliminary Apollo330 Plus datasheet DS-A330PS-0p9p0, are
  recorded for the follow-up release.
- **Not flashed in this cycle.** The `apollo510` and `apollo330` packages were
  not flashed on hardware in this release cycle. They are built from the same
  sources as the Apollo510B package, and the Apollo510B hardware run is the
  smoke test for that shared code. The board-specific paths listed above are
  compile-time and documented, not exercised.

Provenance: owner decisions recorded on issue #33 (2026-09-02), with the figure
sourcing on issues #25 and #18.

### Tiles glossary

**CPU Usage.** Percent busy, computed as `100 - idle` over a 30-second rolling
window (`kCpuStatsRollingSeconds` in `src/main.cc`). It includes everything the
firmware does, which means the USB or BLE transport that exists only to feed
this dashboard is counted in the number. A deployed product that streams nothing
would read lower. BLE reads higher than USB for the same reason: measurements
put BLE at 37.7 percent against roughly 27 to 28 percent for USB (issue #19).
The same number is the busy fraction the battery model bills, so the CPU tile
and the battery tile derive from one measurement. A per-task breakdown and a
deployment projection are emitted on the SWO `cpu` line as diagnostics; see
`docs/developer.md`.

Measured on branch 65-sensor-opt before release, Apollo510B over USB, dashboard
connected, 180 s: 16.0 percent utilization, a 36.4 day battery estimate and
71.3 inferences per second, against 39.9 percent, 24.0 days and 58.5 inferences
per second in the same conditions on the v5.0.0-era build (issue #65). The
v5.0.0 figures quoted in this section stand as the record of that release.

**MCU Battery Life (est., excl. sensor).** This is a **model, not a
measurement**. It covers **MCU energy only; sensor power is deliberately
excluded**, because sensor draw depends on LED count, drive strength, and
sampling duty, none of which are properties of the MCU. The model splits time
into inference, general compute, and sleep, and bills each at its own figure:
sleep and per-MHz compute from the Apollo510B SoC Datasheet DS-A510B-1p1p0
Table 39, inference from bench runlogs dated 2026-02-26. It models about
28 days at 96 MHz: 27.7 days against a measured 30.5 percent busy fraction
(issues #17, #25). The busy fraction it bills is the measured CPU figure above,
so everything the core runs, the demo transport included, is billed at active
power.

It assumes a 1485 mWh budget (2 x 225 mAh at 3.3 V); the cell capacity is a
chosen assumption, not a sourced figure (issue #18). The known errors run
optimistic: the denoise stage has no power measurement and is billed at the
segmentation figure, and the bench figures were taken on `apollo510_evb`.

What it is not: it is not a product battery specification, it is not a system
power measurement, and it is not a single-coin-cell figure. The sleep term is a
projection rather than a measurement of this build, because the demo does not
actually sleep, and it assumes a quiet bus: the sensor task is blocked while the
IOM moves the sensor FIFO, so that transfer time is billed as idle.

**AI Throughput (max sustained).** Inferences per second expressed as
`2e6 / duration`, the scale the host dashboard expects (`ips_from_delta_us` in
`src/main.cc`). This is a **throughput figure, not a run rate**. It answers "how
fast does this model execute when it executes", not "how often does it execute".
The models actually run about once every 2 seconds. Do not read the tile as the
model firing hundreds of times a second.

**Denoise Efficiency (est.)**, **Segment Efficiency (est.)**, and **Arrhythmia
Efficiency (est.)**. AI efficiency in inferences per watt, one tile per model.
Each is throughput divided by the modelled inference power, so all three inherit
the estimate caveat from the battery model: the power term is the same modelled
figure, from the same bench runlogs dated 2026-02-26.

**Speed toggle.** Switches the SoC at runtime between 96 MHz low-power and
250 MHz high-performance operation. **Both modes are supported.** The default is
96 MHz low power.

Measured on an Apollo510B EVB, 2026-09-01 and 2026-09-02 (issue #25), in 250 MHz
high-performance mode expect:

- AI throughput **2 to 3x, varying by model and build**. Two builds of the same
  code measured average throughput of 67.8 against 135.5 inferences per second,
  and 66.2 against 182.6. The models execute in place from MRAM with their
  arenas in shared SRAM, so binary layout changes how the largest model caches
  and moves the three-stage mean without moving inference duty. The bench
  harness, with the models and arena held in TCM, measured 2.59x.
- The three efficiency tiles **change by model in high performance: a tile may
  read higher or lower**. The tiles are throughput divided by inference power, so
  they carry the same binary layout effect as throughput and the three models do
  not move together. Paired low-power and high-performance readings on the
  released v5.0.0 build recorded mixed directions across the three tiles (issue
  #25). The 2026-02-26 bench runlogs measured energy per inference rising about
  17 percent for segmentation in the harness (issue #18); that is a harness
  result, and the on-device tile did not reproduce it. Read the live tiles rather
  than quoting a direction or a percentage in advance.
- A lower battery figure: about 15 days modelled at 250 MHz, 14.6 days against a
  measured 22.3 percent busy fraction, compared with 27.7 days at 96 MHz.

If you are looking at an older build, note that a timebase defect made
high-performance figures read wrong; it was fixed in v5.0.0 (issue #25).

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
  genuine stall is correct behaviour. In hardware validation the demo recorded
  zero visible ECG gaps over a 3-minute baseline and a 10-minute endurance run
  (2026-09-01).

## Troubleshooting

| Symptom | Do this |
| --- | --- |
| Connect fails, device already listed, Scan is greyed out | **Forget Device**, then `usb` -> Scan -> select -> Connect. |
| No device in the chooser | Check the cable is on the **data** connector, not the debug connector, and that the board is powered. |
| Tiles stay at `--` | Confirm Settings -> API Mode is **LIVE**, not Emulate, and reload. |
| Dashboard shows Connected but nothing moves | Replug the data cable, then reconnect. |
| First data after reopening the dashboard looks stale | Close the tab cleanly with Disconnect, or replug the cable. See known limitations. |
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
- **TimedSignal v2 deferred to v5.1 (issue #5, open).** Owner decision,
  2026-09-01. The demo works correctly without it; the streaming fix landed
  separately and is verified on hardware.
- **Battery and efficiency figures are modelled, not measured.** See the tiles
  glossary.

## Documentation

- `docs/fae-runbook.md` is the one-page demo runbook for trade shows.
- `docs/developer.md` explains setup, build, flash, validation, and cleanup.
- `docs/as7058_profiles.md` explains AS7058 sensor profiles and regeneration.
- `docs/design/streaming-pipeline.md` is the streaming pipeline design record.
- `DEVELOPMENT_STATUS.md` records current hardware validation and follow-up
  engineering work.

## License

Ambiq-authored code in this repository is licensed under the BSD 3-Clause
License; see `LICENSE`. The models in `assets/` are covered by the same license
(provenance in `assets/README.md`).

The firmware binaries also contain third-party components under their own
terms. `THIRD-PARTY-NOTICES.md` reproduces those licenses and notices; it is
generated from the modules pinned in `nsx.lock` by
`tools/release/gen_third_party_notices.py` and ships in every release package.

The AS7058 sensor driver is proprietary ams-OSRAM software. It is distributed
in binary form only, as part of the prebuilt firmware, under Ambiq's agreement
with ams-OSRAM; its source is not in this repository. See `NOTICE`.

## Repository Layout

- `boards/` contains the three NSX board definitions.
- `src/` contains application and model-pipeline sources.
- `assets/` contains model, dashboard, stimulus, and AS7058 profile inputs.
- `nsx.yml` and `nsx.lock` define the reproducible NSX dependency closure.

`modules/` and `cmake/nsx/` are generated from the lockfile. Do not commit or
edit their generated contents.
