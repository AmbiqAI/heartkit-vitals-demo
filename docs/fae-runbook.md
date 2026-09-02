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
- [ ] **Mode:** Settings -> API Mode = **LIVE**. If it says Emulate you are
      showing synthetic data. Reload after changing it.
- [ ] **Connect once and let it run for two minutes** before anyone watches.
      Confirm the waveform moves and no tile reads `--`.
- [ ] **Speed toggle left at 96 MHz** (low power). See "what not to claim".

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

**3:00 AI Throughput IPS.**
> "That is how fast each model executes when it runs, not how often. The models
> fire about once every two seconds."

**3:30 IPS/W.**
> "Inferences per watt. This is the number that matters if you are running
> always-on AI on a very small energy budget."

**4:00 CR2032 Battery Life.**
> "27.8 days measured at 96 MHz on this build. That is an estimate assuming
> 1485 mWh, two CR2032 cells, and it covers MCU energy only. Sensor power is not
> in it."

Say the caveat in the same breath as the number, every time. Two cells, never
one.

**4:30 Speed toggle.** Flip to 250 MHz, show that streaming continues, flip
back.
> "The operating point is a runtime control. You trade power for headroom
> without stopping."

The high-performance toggle is being corrected under #25; until it lands, use
low-power mode for the demo. Flip it back to 96 MHz, leave it there, and do not
read the tiles in HP mode.

## What NOT to claim

- **Do not present the battery number as a measurement or a product
  specification.** It is a model of MCU energy only. It excludes the sensor
  entirely. If asked what the whole system draws, say the sensor is not modelled
  and offer to follow up (issues #17, #18).
- **Do not claim battery life on a single coin cell.** The estimate assumes
  1485 mWh, two CR2032 cells. Never say "a CR2032" or "a coin cell" singular.
- **Do not quote any high-performance-mode figure.** In 250 MHz mode the tick
  and DWT timebase do not follow the performance mode, so the AI Throughput IPS
  tiles under-report by 2.604x and the battery tile is affected. Fix is tracked
  in issue #25. The high-performance toggle is being corrected under #25; until
  it lands, use low-power mode for the demo. Use the toggle to show the
  behaviour, read the numbers at 96 MHz.
- **Do not present AI Throughput IPS as a run rate.** It is throughput. The
  models run about once every 2 seconds.
- **Do not present the CPU number as product CPU load.** It includes the demo
  transport. BLE reads about 10 points higher than USB for that reason
  (issues #19, #24).
- **Do not quote sensor power, system power, or battery life for any
  configuration other than 96 MHz.**

## When it misbehaves

| What you see | Do this |
| --- | --- |
| Connect fails, device already listed, Scan greyed out | **Forget Device**, then `usb` -> Scan -> select -> Connect. This is the number one snag. |
| Board not in the chooser | Cable on the **data** connector? Board powered? |
| Tiles stuck on `--` | Settings -> API Mode = LIVE, then reload. |
| Connected but nothing moves | Replug the data cable and reconnect. |
| One blank band in the trace | Normal after a real interruption. The app is being honest about lost data. Keep talking; it recovers by itself and will not flood to catch up. |
| Trace lags you by several seconds | Expected, by design. Say so. |
| BLE will not reconnect | Forget Device, re-scan on `ble`. `apollo510b_evb` only. |
| Nothing works and there is a queue | Replug, reload the browser tab, reconnect. If that fails, reflash from the prebuilt package. |

## Links

- Release and prebuilt firmware:
  <https://github.com/AmbiqAI/heartkit-vitals-demo/releases>
- Full setup and tiles glossary: `../README.md`
- Streaming design record: `design/streaming-pipeline.md`
- Validation evidence: `USB-TEST-RESULTS/RESULTS.md`, held internally and not
  published in this repository. 11/11 pass, zero gaps over 3 and 10 minutes.
- Known limitations:
  [#13](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/13) stale first
  data after an unclean tab close,
  [#25](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/25) HP-mode
  timebase, [#5](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/5)
  TimedSignal deferred to v5.1
- Battery model provenance: issues
  [#17](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/17) and
  [#18](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/18)
