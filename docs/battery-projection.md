# MCU battery projection

Tracking: [issue #68](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/68).

AP510, AP510B, and AP330 share the reference powers below. AP330 enforces LP
operation. Battery life is a projected MCU runtime with quiet idle periods,
not measured runtime of the continuously streaming demonstration.

| Term | LP (mW) | HP (mW) |
| --- | ---: | ---: |
| Quiet sleep | 1.268 | 1.268 |
| Other active work | 4.623 | 14.622 |
| Denoise | 6.577 | 22.099 |
| Segmentation | 5.393 | 17.614 |
| Arrhythmia | 6.265 | 21.112 |

Model references come from the AP510 HPX SRAM/MRAM capture campaign:
scratch in shared SRAM, constants read directly from MRAM, original model
precision. Other active work uses the campaign's spin-loop readings as a
budgeting proxy. The spin probe keeps the debug domain powered; it is not an
application DSP measurement or a baseline subtracted from model power.
Sleep uses the AP510B [memory-bank sweep](../tools/bench/results/lp-power-20260909/sleep-bank-sweep/README.md).

These owner-selected references apply to all three targets, not separate
measurements of each SoC. See [capture provenance](power-reference-captures.md)
for benchmark revisions and measurement scope.

## Calculation

Nominal battery energy is 1,350 mWh: two cells, each assumed 225 mAh at 3 V.
A factor of 0.80 provides the budgeting allowance; it is not measured
regulator efficiency.

`average_mW = (sum(stage_fraction * stage_mW) + other_fraction * compute_mW + idle_fraction * sleep_mW) / 0.80`

`days = 1350 / average_mW / 24`

Stage fractions use completed run counts and the latest measured stage duration
over the CPU utilization window. If their sum exceeds total busy time, they
are scaled proportionally to fit. Other work gets remaining busy time; quiet
sleep gets the idle fraction. The allowance is applied once.

Reference model powers are measured during repeated inference. The demo
applies them to pipeline-stage durations, which also include stage overhead.
Off/DSP processing still contributes to the duty calculation. Sensor supply,
battery usable-capacity characterization, and production sleep transitions are
outside this projection.

## Model-energy tiles

Each model uses its own LP/HP reference power and measured duration. The wire
format stays IPS/W; TileIO converts it to `µJ/inf = 1e6 / (IPS/W)`. Off or
unavailable AI results remain unavailable. The same model references feed
both energy reporting and the battery calculation.

## Validation

Per-board host tests check reference selection, the AP330 LP guard, fixed
workloads, clamping, and invalid AI metrics. Firmware and hardware release
coverage is recorded separately in the release validation record.
