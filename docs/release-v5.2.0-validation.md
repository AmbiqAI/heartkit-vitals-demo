# v5.2.0 validation

## AP510B USB smoke test

Production firmware was programmed and verified through NSX on AP510B,
probe 1160002954. The test used built-in ECG input (Cycle All), LP mode,
and the USB telemetry interface. No Joulescope or power capture was used.

Tested binary SHA-256:
`1c9a800f00021471f829cd5c349218040c3b477e14ce77d6cc720cf54a5aca69`

| Mode | Duration | Packets | CRC errors | AI telemetry |
| --- | ---: | ---: | ---: | --- |
| All AI | 90 s | 2777 | 0 | Three finite model rates |
| All Off | 12 s | 380 | 0 | All AI metrics unavailable |
| All DSP | 12 s | 379 | 0 | All AI metrics unavailable |
| Denoise AI, segment Off, arrhythmia DSP | 12 s | 380 | 0 | Only denoise contributes |
| Restored all AI | 15 s | 475 | 0 | Three finite model rates restored |

All five captures passed. Raw float telemetry was checked for unavailable
model rates and efficiencies, finite battery/CPU metrics, and finite throughput
only when an AI model was available. The mixed-mode average was about 49 IPS.
The board was left in LP/all-AI and the Python USB interface was released.
Local raw logs: `/tmp/hkv-final2-{ai,off,dsp,mixed,restore}.log`.

## Automated checks

- Twelve host tests pass with ASan/UBSan, including an optimized fast-math
  regression for unavailable metrics and partial-model averages.
- AP510, AP330, and AP510B firmware builds pass.
- Paired TileIO PR44: 55 tests and the production web build pass.

## Coverage

This smoke test checks transport, mode control, and AI telemetry. It does not
repeat model numerical parity, qualify live sensor input, validate BLE, measure
power, or establish CPU task stack high-water. Earlier model parity evidence is
in [denoise validation](../tools/aot/denoise-validation.md).
AP510 and AP330 have build coverage only in this release cycle.
