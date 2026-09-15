# v5.2.1 validation

This patch shares the AP510B reference power assumptions across AP510, AP510B,
and AP330. The calculation is tested independently for each board definition.
It does not change sensor pin assignments, clock configuration, transport
support or model execution. This version remained a draft after review found
that AP330's documented LP-only restriction was not enforced. The correction
is recorded in [PR #95](https://github.com/AmbiqAI/heartkit-vitals-demo/pull/95).

## Calculation checks

- Twelve host tests pass with address/undefined-behavior sanitizers.
- Each board variant checks the same LP stage powers, HP reference powers,
  inference-energy constants, battery capacity, and allowance.
- A fixed workload produces the same expected average power and projected
  runtime on all three variants.
- Four AOT Python tests and 132 release-helper assertions pass.

## Hardware coverage

No new hardware or power measurements are recorded for this patch. The shared
profile is an owner-selected budgeting assumption, not proof that the boards
consume identical power. Previous hardware coverage is recorded in the
[v5.2.0 validation record](release-v5.2.0-validation.md).

Firmware build checks for all three targets are recorded in
[PR #94](https://github.com/AmbiqAI/heartkit-vitals-demo/pull/94).
