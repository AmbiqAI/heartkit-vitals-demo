# FAE Demo Runbook

One page. HeartKit Vitals Demo, booth or customer desk, about 5 minutes.

## Pre-show checklist

- [ ] **Board:** `apollo510b_evb` with the AS7058 sensor attached. Use the B
      part; it is the only one with BLE.
- [ ] **Flashed and verified** before the show floor, not on it.
- [ ] **Cables:** data USB cable on the **data** connector. Keep the J-Link and
      the programming/debug cable in the bag as backup, not plugged in.
- [ ] **Browser:** Chrome or Edge. Safari does not support WebUSB.
- [ ] **URL:** <https://ambiqai.github.io/tileio>
- [ ] **Dashboard:** built-in **HeartKit: Vital Sign Monitoring**.
- [ ] **Mode:** Settings, API Mode must be **LIVE** not **Emulate**. Reload after
      changing it. Emulate is synthetic data and is not a demo of this firmware.
- [ ] **Connect:**
      1. Select Device, interface `usb`, Scan.
      2. Pick the Ambiq device (product string `heartkit-vitals-demo`).
      3. Select, then Connect. Wait for Connected.
      4. For BLE, the same four steps with interface `ble` instead of `usb`.
- [ ] **Let it run for two minutes** before anyone watches. Confirm the waveform
      moves and no tile reads `--`.
- [ ] **Speed toggle at 96 MHz** (low power) to start. Both modes work; this is
      just the default you open on.

## 5-minute demo script

**0:00 Open.** "This is an Apollo510B running three ECG models on-device while
streaming to a browser. Everything you see is live from the sensor."

**0:30 ECG Stream.** Put a finger on the sensor. Point at the trace.
> "That is real ECG, captured and denoised on the MCU."

Say up front that the trace runs a few seconds behind you, before they notice it
themselves and think it is broken.

**1:00 ECG Segmentation.** Point at the coloured bands.
> "A second model is labelling P-wave, QRS, and T-wave on every beat, on-device."

**1:30 Vitals tiles.** Heart rate, HRV, pulse rate, SpO2, PPG quality.
> "Standard vitals, derived on the board. No cloud, no phone."

**2:15 CPU Usage.**
> "Read the tile out loud, then say: that includes the USB streaming that only
> exists so you can watch this. A product that just monitored would sit lower."

**3:00 AI Throughput (max sustained).**
> "That is how fast each model executes when it runs, not how often. The models
> fire about once every two seconds."

**3:30 Efficiency tiles.** Three of them: **Denoise Efficiency (est.)**,
**Segment Efficiency (est.)**, and **Arrhythmia Efficiency (est.)**.
> "Inferences per watt, one per model. This is the number that matters if you
> are running always-on AI on a very small energy budget."

**4:00 MCU Battery Life (est., excl. sensor).**
> "About 28 days at 96 MHz. 27.7 days estimated on this build, from a measured
> 30.5 percent busy fraction. It assumes a 1485 mWh budget, two CR2032 cells,
> and it covers MCU energy only. Sensor power is not in it."

Say the caveat in the same breath as the number, every time. Two cells, never
one. Estimated, never measured.

**4:30 Speed toggle.** Flip to 250 MHz, show that streaming continues, flip
back.
> "The operating point is a runtime control. Both modes are supported. At
> 250 MHz you get 2 to 3x the AI throughput, varying by model and build, the
> efficiency tiles always drop, by roughly a tenth to a third depending on the
> model and build, because each inference costs more energy, and the modelled
> battery life falls to about 15 days. You trade energy for headroom, without
> stopping the stream."

If a customer mentions that high-performance figures looked wrong on an older
build, that was a timebase defect, fixed in v5.0.0 (#25).

## What NOT to claim

- **Do not present the battery number as a measurement or a product
  specification.** It is a model of MCU energy only. It excludes the sensor
  entirely. If asked what the whole system draws, say the sensor is not modelled
  and offer to follow up (#17, #18).
- **Do not claim battery life on a single coin cell.** The estimate assumes a
  1485 mWh budget, two CR2032 cells. Never say "a CR2032" or "a coin cell"
  singular. The cell capacity is a chosen assumption, not a sourced figure.
- **Do not present AI Throughput as a run rate.** It is throughput. The models
  run about once every 2 seconds.
- **Do not present the CPU number as product CPU load.** It includes the demo
  transport. BLE reads about 10 points higher than USB for that reason (#19).
- **Do not quote sensor power or whole-system power.** Neither is modelled or
  measured here.
- **Do not present the efficiency tiles as measured.** They divide throughput by
  the same modelled inference power the battery tile uses.
- **Do not quote a specific efficiency percentage in high-performance mode.** The
  drop has measured anywhere from roughly a tenth to a third across builds,
  because binary layout moves throughput. Say only that efficiency is lower in
  high performance, and read the live tile if someone wants a number.

## When it misbehaves

| What you see | Do this |
| --- | --- |
| Connect fails, device already listed, Scan greyed out | **Forget Device**, then `usb` -> Scan -> select -> Connect. This is the number one snag. |
| Board not in the chooser | Cable on the **data** connector? Board powered? |
| Tiles stuck on `--` | Settings, API Mode = LIVE, then reload. |
| Connected but nothing moves | Replug the data cable and reconnect. |
| First data after reopening looks stale | Close cleanly with Disconnect next time, or replug the cable (#13). |
| One blank band in the trace | Normal after a real interruption. The app is being honest about lost data. Keep talking; it recovers by itself and will not flood to catch up. |
| Trace lags you by several seconds | Expected, by design. Say so. |
| BLE will not reconnect | Forget Device, re-scan on `ble`. `apollo510b_evb` only. |
| Nothing works and there is a queue | Replug, reload the browser tab, reconnect. If that fails, reflash from source per README Quick start B: `uv run nsx flash --app-dir . --board apollo510b_evb` |

## Links

- Release and prebuilt firmware (available with v5.0.0):
  <https://github.com/AmbiqAI/heartkit-vitals-demo/releases>
- Full setup and tiles glossary: `../README.md`
- Streaming design record: `design/streaming-pipeline.md`
- Validation evidence: `USB-TEST-RESULTS/RESULTS.md`, held with the validation
  owner, not in this repository. Zero visible ECG gaps over a 3-minute baseline
  and a 10-minute endurance run (2026-09-01).
- Known limitations:
  [#13](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/13) stale first
  data after an unclean tab close,
  [#5](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/5) TimedSignal
  deferred to v5.1
- HP-mode timebase defect, fixed in v5.0.0:
  [#25](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/25)
- Battery model provenance:
  [#17](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/17) and
  [#18](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/18)
- BLE CPU: [#19](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/19)
