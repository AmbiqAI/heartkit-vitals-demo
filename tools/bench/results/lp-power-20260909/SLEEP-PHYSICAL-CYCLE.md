# Physical-cycle comparison

Issue #68. Owner confirmed physical power cycle and both USB cables connected.
Installed image remains the single-bank/low-power-read normal-sleep helper,
SHA256 `cccd6e73f771fa0590ee783ab1f6ea16ec8e4189a42e4b0ec220fb74752db396`.
Joulescope-only recording: no flash, software reset, SEGGER viewer or debug-memory
attachment before, during or after this capture.

| Metric, capture seconds5-30 | Value |
| --- | ---: |
| Mean rail voltage | 1.810665 V |
| Mean current | 0.550558 mA |
| Mean instantaneous power | 0.996894 mW |
| Five-second mean-power range | 0.996689-0.997002 mW |
| Gate high fraction | 100% |

Recording duration34.95s. Gate was already high at start and remained high;
the startup transition was not captured. The hardware sticky flag was not read
because that would require debug attachment. The firmware marker and retained
image identity support the steady-state comparison, not a fresh independent
readout of every power-state register.

No nonfinite analog samples. Analysis uses timestamp-aligned analog intervals
and a full packed-GPI read without deglitching. Raw file:
`sleep-physical-cycle01.jls`. Summary: `sleep-physical-cycle01.csv`.

Compared with two SWPOI/no-viewer runs at0.991233/0.991676mW, physical-cycle
power is about0.5-0.6% higher. This does not show a meaningful software-reset
residual-power penalty in this configuration. It does not validate ordinary
SEGGER debug reset as equivalent; the earlier debug-attached attempt failed
the debug-domain power-off check.

Both USB cables stayed connected. This test does not isolate their contribution
or prove the absence of rail leakage/back-power paths. The claimed measurement
boundary remains the owner's MCU rail, not total EVB/cable-input power.
Leave the board undisturbed until the next agreed comparison.
