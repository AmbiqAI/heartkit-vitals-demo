# MRAM policy sweep, normal sleep

Issue #68. AP510B1160002954, JS110004204, same MCU rail and GPIO wiring as
SLEEP-MINIMAL.md. All captures use clean HPX SWPOI reset, no startup SWO viewer.
Firmware remains LP, normal sleep, small TCM, shared SRAM off, peripherals off.
No production power constants or runtime behavior changed.

| Configuration | Capture | Mean mA | Mean mW | Mean V |
| --- | --- | ---: | ---: | ---: |
| Two MRAM banks, default read policy | sleep-minimal02 | 0.836722 | 1.514824 | 1.810406 |
| One MRAM bank, default read policy | sleep-single-mram01 | 0.675409 | 1.222754 | 1.810363 |
| One MRAM bank, low-power read | sleep-mram-lpr01 | 0.547523 | 0.991233 | 1.810362 |
| Same configuration, independent reset | sleep-mram-lpr02 | 0.547749 | 0.991676 | 1.810425 |

First row uses the previous30-60s interval. Subsequent rows use30-55s after
recording start, aligned across analog channels by JLS timestamps. No nonfinite
analog samples; full packed-GPI reads show every settled-interval sample high.
No deglitching. Power is the mean of instantaneous recorded power.

| Capture | Gate rises at seconds | Five-second mean power range, mW |
| --- | ---: | ---: |
| single-mram01 | 24.73679 | 1.222522-1.223091 |
| mram-lpr01 | 24.73065 | 0.991165-0.991327 |
| mram-lpr02 | 24.74146 | 0.991585-0.991763 |

The repeat starts high from the previous sleep state, drops at2.5179s during
reset, then rises at24.74146s. It was not debug-attached after recording, leaving
the board in its undisturbed test state for the owner's physical-cycle comparison.

## State evidence

After the first single-bank and low-power-read recordings, HPX debug-memory
reads (no reset or halt) confirmed snapshot stage2, normal-sleep SCR0, peripheral
enable/status0, shared SRAM enable/status0, and live sticky CORESLEEP set.
Saved wake status remained0. Both snapshots had MEMPWREN0x09 and MEMPWRSTATUS0x19,
confirming the unused MRAM bank was off. Low-power-read changed
MRAMCRYPTOPWRCTRL from0x200 to0x205, as intended. Other snapshot fields match the
baseline: CPUPWRCTRL0, MEMRETCFG2, SSRAMRETCFG7, VRSTATUS0x30,
OCTRL0x80, CLOCKENSTAT0x44000000. Regulator/clock snapshots are **before WFI**,
not a direct in-sleep measurement. Debug attachment occurred after analog capture.

The SDK normal-sleep HAL path selects ELP retention when starting from ELP_ON;
no separate EPU power-policy change was made. The test does not force MRAM sleep
while executing from it. Remaining difference from the datasheet750uW typical
reference is approximately0.242mW. This is not yet a fully qualified Sleep1 floor.

## Implementation and reproducibility

Test-only switches in tools/bench/sleep_minimal.cmake:

- `HKV_SLEEP_SINGLE_MRAM=ON`: use `AM_HAL_PWRCTRL_NVM0_ONLY`.
- `HKV_SLEEP_MRAM_LOW_POWER_READ=ON`: use the MRAM0 policy from SDK
  `nsx_power_minimize_memory`: LPREN1, SLPEN0, PWRCTRL1, crypto gate retained.

Source of record: vendored nsx-power/src/apollo5/nsx_power.c and Apollo510
CMSIS register definitions. MRAM bank size from HAL am_hal_mram.h. The helper
linker now bounds code/load data to the first MRAM bank, and a negative link test
rejects overflow. Seven layout checks and eight existing host tests pass.
The production target retains its original linker script and still builds.

Binary SHA256:

- Single bank only: `971abf8444d5db8b332d8b74184cd741249ca625595f0480a137b122f4cd4901`.
- Single bank plus low-power read, both repeats:
  `cccd6e73f771fa0590ee783ab1f6ea16ec8e4189a42e4b0ec220fb74752db396`.

Current build cache has both options ON. Raw JLS files retained locally.

## Next: physical-cycle control

Owner proposed physical power cycling to compare against software/debug reset.
Wait for owner confirmation, then capture the installed low-power-read image
using Joulescope only. Do not flash, reset or open a SEGGER viewer beforehand.
The firmware has a20s countdown; a later all-high settled capture is acceptable
as a steady-state power comparison, but will not independently establish the
startup edge. Keep wiring/sensor state unchanged across the comparison.

Later high-level knobs should remain one-at-a-time: reset method, memory
retention/power footprint, then sleep versus deep sleep using supported SDK
entry/restore paths. Active-compute comparisons need a separate controlled
workload. Do not use minimum-memory sleep results directly as full-demo power.
