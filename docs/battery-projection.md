# MCU battery projection

Tracking: [issue #68](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/68).
The owner selected a two-coin-cell ECG wearable scenario. This is a projected
MCU runtime, not the continuously streaming demo's measured battery life.

## AP510B LP assumptions

| Term | Value | Basis |
| --- | ---: | --- |
| Quiet sleep | 1.268 mW | Measured helper with sufficient memory capacity for the demo |
| Other active work | 6.000 mW | Budgeting proxy, rounded from the 5.977 mW spin measurement |
| Denoise stage | 8.968 mW | Measured AOT helper |
| Segmentation stage | 7.443 mW | Measured AOT helper |
| Arrhythmia stage | 8.638 mW | Measured AOT helper |
| Nominal battery energy | 1,350 mWh | Two cells, each assumed 225 mAh at 3 V |
| Allowance | 20% | Existing owner-selected factor of 0.80 |

Sources: [active helper](../tools/bench/results/lp-power-20260909/RESULTS.md),
[sleep bank sweep](../tools/bench/results/lp-power-20260909/sleep-bank-sweep/README.md),
and [streaming demo comparison](../tools/bench/results/active-recheck-20260909/README.md).
The spin value is not a measured mean of application DSP. The allowance is not
a measured regulator efficiency or a substitute for sensor characterization.

## Calculation and scope

The existing stage run counters and last measured stage durations estimate
each stage's wall-time fraction. Denoise, segmentation and arrhythmia fractions
are averaged over the same rolling window as measured CPU busy time. Their
power contributions are summed individually, not equally averaged.

If their sum exceeds busy time, scale all three fractions proportionally to
fit. Other compute gets the remaining busy time; sleep gets one minus busy.

`average_mW = (sum(stage_fraction * stage_mW) + other_fraction * compute_mW + idle_fraction * sleep_mW) / 0.80`

`days = 1350 / average_mW / 24`

The stage duration includes pipeline overhead, not exclusively model invoke.
DSP/off paths still contribute their shorter measured stage times, preserving
the existing duty semantics. The fractions use each stage's latest duration
times the number of runs, not an integrated per-invocation energy counter.
No sleep baseline is added on top of active power, and no blanket spin-minus-
sleep penalty is added. The allowance is applied once.

Model active captures and quiet sleep used different configurations. The
projection assumes production-style idle management; no actual sleep entry,
memory-bank policy, sensor timing or streaming behavior is changed. Sensor
supply energy and usable coin-cell capacity remain outside this validation.
Deep sleep is not assumed.

Only the AP510B LP battery profile changes. HP and the AP510/AP330 profiles use
their existing constants. The three IPS/W metrics use conventional inferences
per second and retain their separate power assumptions. Stage duty is
`runs / (IPS * window_seconds)`, so normalizing throughput does not change
the battery projection.

## Implementation and validation

- src/battery_model.h contains the profile selection and weighted calculation.
- src/main.cc keeps per-stage duty histories synchronized with CPU utilization.
- tests/test_battery_model.c covers profile selection for all three boards,
  capacity, idle/compute cases, unequal stage weights, proportional clamping,
  negative/oversized fractions and unchanged HP assumptions.
- All eleven host tests pass with address/undefined-behavior sanitizers.
- AP510B, AP510 and AP330 firmware builds pass through the board-aware NSX
  build path. Direct reuse of a CMake directory after changing boards can
  encounter the other board's generated module list; regenerate through NSX.
- AP510B was programmed and verified. During a 16.7-second USB run, it reported
  approximately 23.6–24 days at roughly 11–12% busy, with 464 packets and no CRC
  errors. Sensor/model counters showed no errors before the fixture disappeared.
- The intended 65-second USB test did not finish: the target, probe and JS110
  all disappeared from USB. Full rolling-window and mode-toggle hardware
  validation remain pending reconnection. Logs are in
  tools/bench/results/battery-projection-20260909.

Final AP510B build SHA256:
`edccb254be16fe22b1a294d85fa27078c854664f64289a1c131b64fe8604ced1`.
Source-comment cleanup was rebuilt after the fixture disconnected; that final
artifact was not reflashed. No functional code changed after the hardware run.

The approximately 22-day planning example assumed higher busy time. The
dashboard value is calculated from measured workload, not fixed to that example.
