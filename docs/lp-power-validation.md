# LP demo power sanity check

Tracking: https://github.com/AmbiqAI/heartkit-vitals-demo/issues/68

## Goal and status

Check whether the demo's projected MCU
battery life is in the right ballpark, without redesigning the streaming demo.
Start with Apollo510B, LP only. Do not change production idle behavior as part
of this experiment. The simplified first pass is implemented and hardware-tested;
see ../tools/bench/results/lp-power-20260909/RESULTS.md. Extended tests below
remain deferred unless covered by a later report. Subsequent sleep-bank results
are linked from [minimal sleep validation](minimal-sleep-validation.md).
For the installed image and ongoing measurements, consult ../HANDOFF.md.

Battery budget corrected locally to 2 x 225 mAh x 3.0 V = 1350 mWh. The cell
charge capacity remains a demo assumption. The manufacturer's nominal voltage
is 3.0 V: https://data.energizer.com/pdfs/cr2032.pdf. This is nominal energy,
not a guarantee of usable energy under load or after regulation.

## Simplified first pass (owner scope)

Owner confirms JS110 measures the 1.8 V MCU rail, not the entire EVB input.
Treat cross-rail leakage as a caveat, not a separate characterization project.
Prioritize active compute power. Defer deep sleep and scheduled application
replay below unless the simple captures show they are needed.

Use test-only helper functions before normal application bring-up:

- LP awake spin loop as the idle baseline (not sleep).
- Repeat each of the three production AOT model calls over a marked window,
  using recorded input and existing memory placement. Report absolute mean
  power and energy per call, with the spin baseline alongside it.
- Light sleep with a timer wake, retaining the same working memory.

Skip USB, BLE/radio stack and sensor initialization for these tests. Do not
assume using USB prevents BLE overhead: src/store.c enables both, and main.cc
initializes BLE independently. Verify radio power/clock shutdown explicitly;
the Apollo5 power implementation does not consume need_ble. Do not rely on
that configuration flag alone. A normal-demo capture can be compared later.

One marker is sufficient, with ordered windows and the phase manifest saved
by Python. Apollo510B EVB Quick Start Guide v1.1, p14 Figure7:

| EVB | JS110 | Purpose |
| --- | --- | --- |
| J8 pin1, GP0 | IN0 | Window gate |
| J8 pin14, GND | GND | Logic reference |

Leave JS110 +5V and OUT0/OUT1 unconnected. Configure its logic level to 1.8 V
in software, not by wiring a supply to the GPIO connector. JS110 GPIO ground
is USB-referenced; this plan assumes high-side current sensing on the positive
MCU supply rail so the ground connection cannot bypass a low-side shunt.
Source: https://download.joulescope.com/docs/JoulescopeUsersGuide/JoulescopeUsersGuide_v1_1.pdf

## Deferred extended tests

1. **Streaming baseline, 60 seconds.** Run the existing LP demo with all three
   AI modes enabled and the normal browser transport. Capture measured mean
   voltage, current, power, total energy, and firmware utilization over the
   same window. Record which sensor and transport are active. This is demo
   consumption, not a sleep-capable product estimate.
2. **Quiet-state sweep, 10 seconds per state, three repetitions.** A separate
   test entry point runs awake idle, normal sleep, then deep sleep. Do not
   initialize USB, BLE or the sensor interface. Disable periodic logging and
   SysTick for the quiet windows; use a verified low-power timer to wake.
   Keep the production model memory retained, not a minimum-memory SDK profile.
   Capture stable floor separately from entry/exit energy. Check the timer
   advanced and firmware resumed on every repetition. Compare normal and deep
   sleep with the same peripheral and retention configuration.
3. **Work-and-sleep replay, 60 seconds per sleep policy.** Reuse recorded ECG
   inputs and the production model wrappers, memory placement and invocation
   schedule. Run inference bursts separated by timer sleep, once with normal
   sleep and once with deep sleep. Include wake/restore energy and verify
   outputs and invocation counts. Keep USB/BLE and the physical sensor off.
   This validates model-work plus idle energy, not a full sensor application.

The burst test is not another model benchmark campaign. Existing model energy
results are inputs only when their target, rail, placement and capture windows
match. The integrated capture checks the missing sleep and wake costs.

## Synchronization and capture

- Firmware owns the timing. A GPIO gate rises at the measured window start
  and falls at its end; capture it on Joulescope IN0 with current and voltage.
  A second marker can identify active work versus sleep in replay. Select
  accessible, unused GPIOs only after checking the connected EVB and logic rail.
- Start Python recording before the firmware's startup delay expires. No USB
  command or SWO print inside quiet measurement windows. Log window identity,
  wake reason, invocation counts and output checks after the gate closes.
- Integrate power over the matching sample interval, not host callback arrival
  times. Save raw JLS plus a small CSV of window duration, mean V/I/P, energy,
  charge, state and build identity. Reject missing edges, sample gaps and
  incorrect window lengths. Include sleep transitions in replay totals.
- Vendor Python support already covers GPIO-triggered capture and recording:
  https://github.com/jetperch/pyjoulescope_examples/blob/main/bin/trigger.py
  https://joulescope-driver.readthedocs.io/en/latest/py_api.html
- Local NSX has `nsx_power_deep_sleep()` and power-monitor GPIO helpers. Normal
  sleep can use the SDK sleep entry point. Review wake-source retention before
  implementing either. Do not use blanket shutdown or memory-minimization
  helpers that remove the wake timer or live model buffers.

## Measurement boundary

JS110, MCU 1.8 V rail, as confirmed by owner. Cross-rail leakage remains possible.
Disconnect the debug session for sleep measurements. Keep sensor and transport
configuration fixed across the isolated test windows.

Do not subtract guessed EVB overhead or add a regulator margin to a measurement
that already includes that regulator. Do not combine MCU-only sleep power with
whole-board active power as if they were the same measurement boundary.

## Acceptance and claims

Report each repeated result and spread. Investigate a modeled-versus-measured
gap above 20% as a proposed engineering sanity threshold, not instrument error.
Use measured mean power at one consistent boundary to calculate nominal-energy
runtime. Show streaming-demo consumption and sleep-capable projection separately.
No coin-cell runtime guarantee: cutoff, pulse loading, sensor consumption and
conversion losses still need their own allowance or measurement.
