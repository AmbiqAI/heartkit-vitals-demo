# Full-demo memory bank requirements

Issue #68. September 9, 2026, Apollo510B build in this worktree. These are
linker allocation/placement results, not measured peak allocator or stack use.
Source artifacts: `build/apollo510b_evb/heartkit-vitals-demo`, its `.map` and
`.bin`, inspected with the configured GCC size/objdump/nm tools.
This snapshot precedes the battery-projection change; its binary SHA256 is
`b00d8f79810f95b611fad18c52bffdd32cd8daed9f81e16efe66ba0016e85c9f`.

## Current placement

| Memory | Allocated / image span | Smallest supported powered configuration that fits |
| --- | ---: | --- |
| ITCM | 28 bytes | Coupled to DTCM selection below |
| DTCM, static allocations including stack and RTOS pool | 282,744 bytes / 276.12 KiB | 256 KiB ITCM + 512 KiB DTCM |
| Shared SRAM | 113,200 bytes / 110.55 KiB | One 1 MiB group |
| MRAM application image | 741,088 bytes / 723.72 KiB | One 2 MiB bank |

The HAL offers TCM pairs of32/128,128/256,256/512 KiB (ITCM/DTCM). They are
not independently selectable in this interface. DTCM allocations exceed256KiB
by20,600 bytes /20.12KiB, so the middle pair does not fit the unchanged layout.
The default application linker exposes496KiB of the512KiB DTCM configuration.

The shared-SRAM data occupies0x20080000 through0x2009ba30, entirely inside
group0. The MRAM image starts at0x00410000 and ends at0x004c4ee0, entirely in
bank0; the SBL reservation below the application is also within that bank.
The MRAM total includes initialized RAM load images, not just executable code
or model weights. ELF debug sections do not consume target MRAM.

## What the RAM totals include

| Allocation | DTCM bytes | Shared-SRAM bytes |
| --- | ---: | ---: |
| Main stack reservation | 16,384 | 0 |
| Initialized data | 2,520 | 0 |
| Zero-initialized data, fixed pools and buffers | 263,840 | 0 |
| Initialized shared allocations, including AOT arenas | 0 | 105,008 |
| USB DMA buffers | 0 | 8,192 |

The76KiB FreeRTOS heap pool (`ucHeap`) is already included in the263,840-byte
row. Task stacks and queues allocated from it must not be added again.
The three AOT arenas total103,136 bytes /100.72KiB and are already included
in shared SRAM. See minimal-sleep-validation.md for individual buffer sizes.

The linker also reserves225,160 bytes from the end of static DTCM allocations
to its region limit as newlib allocator headroom. This is not evidence that
the application uses another219.88KiB at runtime. Do not trim the RTOS pool or
stack reservations based only on this map; high-water measurements are pending.

## Power-test implication

The unchanged full-demo placement can use one shared-SRAM group and one MRAM
bank, rather than the3MiB shared-SRAM/two-MRAM policy selected by the existing
NSX power configuration. Keep the larger TCM pair for now. This is static-fit
evidence, not hardware validation of a changed production memory policy.

The representative sleep helper was tested with:

- 256KiB ITCM +512KiB DTCM;
- 1MiB shared SRAM, covering the model arenas and USB DMA buffers;
- MRAM bank0, with the tested low-power read policy.

This combination measured **1.268080 mW / 0.700403 mA** on the MCU rail.
See the [controlled bank-sweep report](../tools/bench/results/lp-power-20260909/sleep-bank-sweep/README.md).
This validates the helper's powered-bank state, not a changed production policy.

It need not run the application to measure that retained-memory floor. It also
must not be described as the streaming demo's consumption: USB/BLE/sensor and
application wake behavior require separate measurement.

Optional later optimization: relocate or trim at least20.12KiB of DTCM data,
plus any desired allocator margin, to try128KiB ITCM +256KiB DTCM. There is ample
space in the already-required shared-SRAM group, but such moves need access,
DMA and model-performance validation. No placement changes made here.

## Sources of record

- `src/store.c`: `nsxPwrCfg`, `small_tcm=false`, `need_ssram=true`.
- `config/FreeRTOSConfig.h`: Apollo510B76KiB RTOS pool and dynamic allocation.
- Vendored `nsx-power/src/apollo5/nsx_power.c`: full-memory configuration mapping.
- Apollo510 HAL `am_hal_pwrctrl.h`: TCM pairs and1MiB shared-SRAM groups.
- Apollo510 HAL `mcu/am_hal_mram.h`: 2MiB MRAM bank size.
- Apollo510B GCC linker script: region origins/lengths and heap reservation.

These SDK files are under `modules/nsx-ambiq-sdk/modules/`; exact linked sizes
come from the ELF/map above. This analysis does not change firmware or claim
release readiness.
