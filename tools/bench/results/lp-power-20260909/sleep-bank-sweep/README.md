# Controlled AP510B memory-bank sleep sweep

Issue [#68](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/68).
September 9, 2026. Seven configurations completed, one60s capture per setting;
reported interval30-55s. This measures quiet normal-sleep bank costs, not
deep-sleep leakage or a running application's consumption.

## Results

| Case | TCM total KiB | SRAM MiB | Mean mA | Mean mW | Five-second mean range mW |
| --- | ---: | ---: | ---: | ---: | --- |
| tcm160-sram0 | 160 | 0 | 0.549621 | 0.995082 | 0.995002-0.995172 |
| tcm384-sram0 | 384 | 0 | 0.572619 | 1.036714 | 1.036515-1.036948 |
| tcm768-sram0 | 768 | 0 | 0.611019 | 1.106174 | 1.106076-1.106236 |
| tcm160-sram1 | 160 | 1 | 0.637987 | 1.155012 | 1.154715-1.155319 |
| tcm160-sram2 | 160 | 2 | 0.726913 | 1.316267 | 1.315907-1.316459 |
| tcm160-sram3 | 160 | 3 | 0.814171 | 1.474130 | 1.473836-1.474399 |
| tcm768-sram1 | 768 | 1 | 0.700403 | 1.268080 | 1.267701-1.268383 |

TCM pairs (ITCM +DTCM) are32+128,128+256,256+512KiB. These are coupled HAL
selections. SRAM settings enable/retain the first zero, one, two or three1MiB
groups. The helper itself always fits the smallest memory configuration.

With shared SRAM off,160->384KiB TCM added41.632uW;384->768KiB added69.461uW.
With TCM fixed at160KiB, successive1MiB SRAM groups added159.930,161.256 and
157.862uW. Keeping one rather than three groups saved319.118uW in that sweep.

The full-demo-capacity combination (768KiB TCM,1MiB shared SRAM, one MRAM bank)
measured1.268080mW /0.700403mA. It powers sufficient memory for the linked demo
without relocating allocations. It does not run the demo or reproduce its data
contents, peripheral activity, wake rate or transport load.

Interpretation: removing unused SRAM groups provides a larger sleep saving
than the measured largest-to-middle TCM change. Trying the middle TCM pair in
production requires relocating/trimming at least20.12KiB DTCM plus chosen
headroom, with access/DMA/inference validation. The TCM saving above was measured
with SRAM off; do not present it as a directly measured full-demo saving.

## Fixed conditions and validation

AP510B probe1160002954, JS110004204 on the owner's MCU positive1.8V rail.
Both USB cables remain connected. LP mode, normal WFI, cache enabled, MRAM bank0
only and MRAM low-power read policy enabled. BLE enable low and exported clock
pin disabled. Same20s countdown, clean HPX SWPOI reset, no SWO/debug attachment
during recording. No production power constants changed.

All seven accepted results have:
- A rising gate edge after startup, and every settled gate sample high.
- Finite timestamp-aligned voltage/current/power samples.
- Snapshot stage2, normal-sleep SCR0, peripheral enables/status0.
- TCM and SRAM power masks matching the requested configuration.
- MRAM control0x205, BLE enable output-latch0 and pad-input0.
- Sticky CORESLEEP set after capture, saved wake status0.

No analog filtering or GPI deglitching. Gate analysis reads the whole packed
signal before slicing. Five-second means describe within-capture stability,
not instrument accuracy or independent-run reproducibility. Approximate deltas
are suitable for this board's engineering comparison, not silicon guarantees.

Debug-memory reads happened after each recording, never inside the analog
window. The last action was another clean SWPOI reset after those reads, with
no viewer attached. The installed image is the full-demo-capacity sleep helper,
TCM768/SRAM1, not the production application.

## BLE evidence and boundary

The AP510B BSP maps EM9305 ENABLE toGPIO93. The helper drives it low and enables
the input receiver with READPIN selected. Every pre-WFI snapshot contains both
the output latch and the pad input as0. This is stronger than only recording
the software's requested output value. Clock exportGPIO138 is disabled, matching
the SDK device driver's disable sequence.

The [EM9305 datasheet v4.5.3, Table6-4 and chip-disable description](https://www.emmicroelectronic.com/sites/default/files/products/datasheets/EM9305-DS.pdf)
defines ENABLE-low as chip-disable, with functions disabled and no CPU clock.
Thus the measured GPIO state satisfies the documented controller-disable
condition. This is not an independent internal CPU trace, a measurement at the
BLE die's pin, or an isolated BLE-supply current measurement. Cable/rail leakage
has not been ruled out.

Source of record for mappings and control behavior:
- SDK boards/apollo510b_evb/bsp/am_bsp_pins.h.
- SDK devices/am_devices_em9305.c, am_devices_em9305_disable().
- SDK Apollo510 HAL am_hal_gpio.h, input/output read and READPIN configuration.
- SDK Apollo510 HAL am_hal_pwrctrl.h, supported memory selections.

These SDK paths are under modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk.

## Reproduction and artifacts

Run tools/bench/run_sleep_bank_case.py with --tcm160/384/768, --sram0/1/2/3,
and a unique --name (separate option/value arguments). Supply --hpx-python
with the heliaPROFILER environment's Python executable, --probe-serial and
--joulescope-serial for the connected fixture. Prepare the AP510B build through
NSX first if another board was built last. Enable the JS110 current path before
starting; the sleep runner preserves its existing range setting. It configures/builds,
checks memory layout, flashes explicitly selected probe, records JS110, performs
SWPOI reset, then validates analog data and reads the post-capture snapshot.

Each case folder holds capture.jls, result.json (including binary SHA256),
and configure/build/layout/flash/reset/record/state logs. Raw JLS/logs remain
local and ignored by git. summary.csv contains all numeric results.
The runner refuses an existing case directory and stops at a failed check.
Seven layout/negative-link checks and eight existing host report tests pass.

Final installed binary SHA256:
4f0b4d1663164693167dabd2e6cf79cc3fd3f4cabe5e3256780cb768850f9e0d

## Next

Do not substitute the tiny baseline into the dashboard. The representative
memory-only normal-sleep floor is the full-demo-capacity row above, still without
USB/BLE/sensor activity. Any deep-sleep comparison should preserve the same
required memory and verify safe entry/return separately. Production bank-policy
changes need linker bounds plus hardware regression, not just these helper tests.
