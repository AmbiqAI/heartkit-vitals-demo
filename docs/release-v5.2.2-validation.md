# v5.2.2 validation

This patch shares the AP510B reference power assumptions across all three
targets and enforces LP mode on AP330. AP510 and AP510B retain LP/HP switching.

## Automated checks

- Twelve sanitized host tests, four AOT Python tests, and 132 release-helper
  assertions pass.
- Each board checks the same power assumptions and fixed-workload projection.
- All 256 speed-request byte values are checked for each board: AP330 remains
  LP; AP510 and AP510B preserve their LP/HP selection behavior.
- All three firmware targets build. CI results are tracked in
  [PR #95](https://github.com/AmbiqAI/heartkit-vitals-demo/pull/95).

## AP510B USB smoke test

The AP510B firmware from commit `e3e726b` was flashed and verified with J-Link.
The Python USB client enabled all three AI models and captured each mode for
35 seconds:

| Mode | Packets | CRC errors | State response |
|---|---:|---:|---|
| LP | 1,043 | 0 | LP, all models AI |
| HP | 1,144 | 0 | HP, all models AI |
| Return to LP | 1,106 | 0 | LP, all models AI |

ECG, PPG, CPU, and model metrics were received. The board was left in LP with
all models enabled. This is a transport/control smoke test, not a numerical
model-parity test or a power measurement.

AP510, AP330, and BLE were not hardware-tested for this patch. The owner
approved release without additional hardware coverage. The shared power
profile remains a budgeting assumption, not evidence of equal measured
consumption across boards.
