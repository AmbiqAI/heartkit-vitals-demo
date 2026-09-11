# AP510B stripped sleep preparation

Tracking: [issue #68](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/68).
Prepared September 9, 2026. Owner reconnected the board; two attempts completed.
The clean-reset run reached normal sleep: **0.8367mA /1.5148mW at1.8104V**.
The first attempt stopped because the debug domain stayed powered. See the
[hardware report](../tools/bench/results/lp-power-20260909/SLEEP-MINIMAL.md).
The result is not yet a qualified datasheet Sleep1 floor. No scheduled testing.
Follow-up MRAM sweep reached0.9912/0.9917mW in two clean-reset runs with one
bank and low-power read enabled. See [MRAM sweep](../tools/bench/results/lp-power-20260909/SLEEP-MRAM.md).
The subsequent seven-case [memory-bank sweep](../tools/bench/results/lp-power-20260909/sleep-bank-sweep/README.md)
passed hardware checks. The configuration with enough memory for the full demo
measured **1.2681 mW**, without running the application or its peripherals.

## What this test establishes

`hkv_sleep_minimal` is a separate bare-metal image. It does not initialize
models, FreeRTOS, USB, BLE, or the sensor. It requests LP mode, powers down
every HAL-listed peripheral with checked return status and readback, and holds
the EM9305 enable low. Debug remains available for the startup countdown only.
All IOMs are included. It stops the general timers, STIMER and SysTick, disables
and clears NVIC interrupts, and disables debug before normal sleep.

It selects `PWRCTRL_CPUPWRCTRL_SLEEPMODE_AMBIQ_SLEEP`, described by the SDK as
gating CLKIN. The alternate ARM sleep selection leaves CLKIN running and relies
on internal core clock gating. This is an explicit configuration, not a finding
that the previous image used the wrong selection.

The linker restricts this executable to 32 KiB ITCM, 128 KiB DTCM and zero shared
SRAM. Selectable powered capacity is 160/384/768 KiB total TCM and 0/1/2/3 MiB
shared SRAM; the linker always keeps helper allocations in the smallest regions.
Memory configuration and TCM/shared-SRAM power readbacks are checked.
Cache stays enabled under the SDK normal-sleep policy. Two test-only options
select a single MRAM bank and its SDK low-power read policy. They default OFF
for the original baseline; the follow-up build enables both. No manual MRAM
sleep command is issued while executing from MRAM. No EVB supply-switch GPIO
or blanket GPIO tristate is used.

## Datasheet comparison, not a guaranteed measurement

Source: Apollo510B SoC Datasheet v1.1.0, page 216, Table 39
(`Apollo510B-SoC-Datasheet.pdf`).

| Mode | Typical power at 1.8 V | Equivalent current |
| --- | ---: | ---: |
| System Sleep 1, 160 kB TCM retained | 750 uW | 416.7 uA |
| Deep Sleep 2, 160 kB TCM retained | 23.8 uW | 13.2 uA |
| Deep Sleep 2, all shared SRAM/TCM retained | 57 uW | 31.7 uA |

Current is calculated as power / 1.8 V. These are different states, not
interchangeable targets. This image prepares **normal Sleep 1**, not deep sleep.
The Sleep 1 row requires gated core/peripheral clocks, HFRC on, XTAL off,
SIMO buck enabled, NVM standby, cache retained and no shared SRAM. The image
matches the requested memory footprint; clock/regulator/NVM behavior still needs
hardware confirmation. Saved register snapshots support that investigation.

The previous capture was 3.67060 mW / 2.02759 mA at 1.81039 V. It retained full
memory, including shared SRAM. Its gate was set before calling the HAL, so a
high gate proves no observed return, not necessarily reaching WFI. Do not use
that capture to conclude this datasheet state is unattainable or to replace the
stored sleep constant. See the prior
[capture report](../tools/bench/results/lp-power-20260909/SLEEP-BASELINE.md).

## Linked memory breakdown

Source: GCC ELF/map files in `build/apollo510b_evb`, September 9 local builds.
Sizes are bytes; KiB = 1024 bytes. Debug information in the ELF is not flashed.

| Allocation | Stripped sleep | Full demo |
| --- | ---: | ---: |
| ITCM code | 28 | 28 |
| Main stack reservation | 16,384 | 16,384 |
| Initialized DTCM data | 968 | 2,520 |
| Zero-initialized DTCM data and fixed pools | 22,072 | 263,840 |
| **Static DTCM total including stack** | **39,424 (38.50 KiB)** | **282,744 (276.12 KiB)** |
| Allocator headroom reserved to region end | 91,648 | 225,160 |
| Shared SRAM, initialized allocations | 0 | 105,008 |
| Shared SRAM, USB DMA buffers | 0 | 8,192 |
| **Shared SRAM total** | **0** | **113,200 (110.55 KiB)** |

The allocator-headroom row is not measured live allocation. Do not treat the
linker's `.heap` fill as additional application data. Likewise, the demo's
76 KiB FreeRTOS pool is already inside its zero-initialized data total; task
allocations inside that pool must not be counted twice. Stack high-water marks
and allocator use require hardware execution and are not reported here.

Selected contributors, already included above:

| Full-demo allocation | Bytes | Location |
| --- | ---: | --- |
| FreeRTOS pool (`ucHeap`) | 77,824 | DTCM |
| HAL IOM handles | 18,048 | DTCM |
| PPG peak state | 16,000 | DTCM |
| Three metric ring buffers | 24,000 | DTCM |
| BLE WSF pool | 6,560 | DTCM |
| Denoise AOT arena | 65,600 | Shared SRAM |
| Segmentation AOT arena | 22,560 | Shared SRAM |
| Arrhythmia AOT arena | 14,976 | Shared SRAM |

The three AOT arenas total 103,136 bytes (100.72 KiB). The full demo cannot
use the 160 kB TCM / zero-shared-SRAM profile without changes. It does fit within
the datasheet's all-memory retention capacity, but that does not demonstrate
the corresponding deep-sleep power or safe application suspend/resume.

## Prepared verification

From this worktree:

```sh
cmake -S . -B build/apollo510b_evb -DHKV_BUILD_LP_POWER_TEST=ON \
  -DHKV_SLEEP_SINGLE_MRAM=ON -DHKV_SLEEP_MRAM_LOW_POWER_READ=ON \
  -DHKV_SLEEP_TCM_KIB=160 -DHKV_SLEEP_SRAM_MIB=0
cmake --build build/apollo510b_evb --target hkv_sleep_minimal -j 6
.venv/bin/python tools/bench/check_sleep_minimal.py build/apollo510b_evb
```

Seven checks pass: powered-memory ELF placement and strong pre-WFI hook, unchanged
production/reference linker scripts, a valid small link, rejection of DTCM
overflow, rejection of ITCM overflow, rejection of any shared-SRAM allocation,
and rejection of code/data extending beyond MRAM bank zero.
The hook is immediately followed by WFI in the linked normal-sleep HAL path.
Eight existing power-report/denoise-report host tests pass. Production target
still builds; no production sleep or power-constant change was made here.

Binary: `build/apollo510b_evb/hkv_sleep_minimal.bin`.
Historical MRAM-sweep single-bank/low-power-read SHA256:
`cccd6e73f771fa0590ee783ab1f6ea16ec8e4189a42e4b0ec220fb74752db396`.
The original baseline hash is preserved in the hardware report. Per-case hashes
and the installed TCM768/SRAM1 image are recorded in the memory-bank sweep report.

## When the owner reconnects

1. Confirm AP510B and the sensor disconnected, same high-side JS110 measurement
   on the MCU 1.8 V rail. Preserve J8 pin 1 / GP0 to JS110 IN0 and J8 pin 14 / GND
   to JS110 GND. Set JS110 logic reference to 1.8 V. Do not attach JS110 outputs.
2. Start recording, then flash this test only. Select probe explicitly after
   checking identity. Previously used AP510B probe: 1160002954; JS110: 004204.
   `nsx flash --board apollo510b_evb --target hkv_sleep_minimal --probe-serial 1160002954 --frozen`
3. Capture startup status/countdown briefly, then detach the debug viewer before
   the 20-second countdown ends. Stop if configuration/readback fails.
4. GP0 rises inside the pre-WFI hook after the HAL sleep preparation. Capture
   at least 30 settled seconds. A return clears the gate and prints
   `UNEXPECTED_WAKE`. No intentional wake source is armed; recovery uses reset.
5. Stop analog recording before debugger inspection. Inspect `g_sleep_snapshot`
   (stage 2 means pre-WFI hook reached) and the sticky `SYSPWRSTATUS.CORESLEEP`
   bit without resetting first. Entry flags are cleared immediately before sleep.
   If an unexpected return occurred, `g_sleep_wake_status` preserves status.
   Debug attachment can wake/disturb the MCU, so it is outside the measured window.
6. Compare mean power and stable blocks against the Sleep 1 target, with the
   actual register state attached. If still high, inspect clock requests,
   regulator state and NVM standby first. Do not broaden into deep-sleep changes
   or change dashboard numbers until this baseline is understood.
