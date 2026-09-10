# Minimal no-wake light-sleep capture

Issue #68. AP510B, JS110 serial004204 on the owner's MCU supply rail.
Requested sensor-disconnected baseline; physical disconnection is not observable
from software. Test target `hkv_sleep_baseline`, source `tools/bench/sleep_baseline.c`.

| Measurement | Result |
| --- | ---: |
| Mean rail voltage | 1.81039 V |
| Mean current | 2.02759 mA |
| Mean power | 3.67060 mW |
| Analyzed settled interval | 32.2 s |
| Five-second block means | 3.66935-3.67256 mW |

The gate rose approximately31.6 seconds into the recording and stayed high to
the end. Analysis excludes the first second after that transition and uses
capture32.6-64.8 seconds. Every gate sample in that settled interval was high;
no gate deglitching was necessary. No nonfinite voltage/current/power samples
were present in the analyzed interval. Power is the mean of the recorded
instantaneous power samples, not a product of mean current and voltage.

Firmware prints a countdown, then "Entering sleep", disables debug output,
SysTick, STIMER clock/interrupts and NVIC interrupts, and calls normal WFI sleep
once. GPIO0 goes high just before the sleep call. If it returns, GPIO0 goes low
and firmware enables repeated UNEXPECTED_WAKE printing. The sustained high gate
indicates no observed return during capture. The debug viewer was detached
before entry, so absence of the final print is not an independent observation.
The marker also cannot rule out a stall inside HAL preparation before WFI.

No model, USB, BLE stack or sensor initialization. EM9305 enable held low and
its IOM disabled. Full memory power/retention policy deliberately matches the
previous test; this is not a minimal-memory or qualified datasheet Sleep1 test.

The prior timer-woken light-sleep result was3.78270 mW; this is about0.1121 mW
(3.0%) lower. More than one condition changed, so do not attribute that difference
solely to the sensor or the timer. The stored assumption remains0.75 mW.

Evidence: sleep-baseline01.jls (raw local capture), sleep-startup01.log (LP
countdown observed), both in this directory. Firmware binary SHA256:
`5752cdd027ef35704c1d82e1012eef077a0400d0f717e5038b490d28b4ee4adb`.

The board was left running this sleep test, not restored to the demo. No power
constant or dashboard value was changed from these measurements.
