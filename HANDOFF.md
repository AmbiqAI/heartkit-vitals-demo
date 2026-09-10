# AOT migration and LP power validation

## Goal and boundaries

Complete the AOT migration and prepare local changes for review under issues
#37 and #68. Owner approved branch push and draft PR update; those are published.
No merge, release, packaging or scheduled work without approval.
Work only in this worktree and the paired TileIO worktree.

Branch: work/aot-021-denoise. Base: 04207dc. Migration commits: d0a4afc and
cb5c565. Existing draft PR: https://github.com/AmbiqAI/heartkit-vitals-demo/pull/85.
PR85 now includes implementation head3de37f6 and links TileIO draft PR44.
Its earlier passing CI applies to cb5c565; check hosted CI on the published head.
Local commits are approved; consult git log for their final IDs.

## Done

- IPS normalization uses 1e6/duration_us for all three models and 1/IPS for
  stage duty. Live IPS and IPS/W are halved; battery duty/projection, model
  latency and relative gains are unchanged. Shared inference_timing.h is used
  by firmware and host regression tests. Scale-compatibility notes removed.
  Historical capture figures remain records of their original builds.
  All three board builds and eleven sanitizer tests pass after this correction.
  Corrected binaries have not been flashed; older hardware evidence does not
  validate this image. Dashboard needs no numeric conversion or range change.

- Production denoise uses AOT 0.21.0; segmentation/arrhythmia retain accepted
  AOT 0.19.0 outputs. ns-cmsis-nn 7.32.0; production manifest/lock excludes
  heliaRT/TFLM. Denoise linkage workaround tracks helia-aot#407.
- AP330 Rev2 click mapping: IOM2, SCL25/SDA26, INT107; explicit BSP click-pin
  configuration and matching ISR. AP510/AP510B retain IOM1, SCL8/SDA9, INT50.
  Source and FAE checklist: docs/ap330-evb-validation.md. R1 testing discontinued.
- AP510B LP battery projection uses measured quiet sleep 1.268 mW, other-work
  proxy 6 mW, and stage powers 8.968/7.443/8.638 mW for denoise/segment/arrhythmia.
  Stage duty and CPU busy fractions use aligned rolling windows.
- Two-cell nominal energy: 1350 mWh, with margin 0.80. No actual idle/sleep or
  production memory policy change. HP/other-board power profiles and IPS/W
  denominators unchanged; AP330 power sourcing remains issue #71.
- src/battery_model.h and README explain the calculation and measurement scope.
  docs/battery-projection.md holds the detailed assumptions and limits.
  HPX latency/memory benchmarks are separate from the JS110 battery captures.
- Opt-in LP/sleep helpers, bank/layout checks and capture/report scripts are
  included with compact results. Raw JLS/logs remain local and ignored.
  Capture scripts require explicit instrument serials; sleep runner also
  requires --hpx-python pointing to the heliaPROFILER environment.

## Verified versus pending

Earlier same-device AOT/TFLM validation passed 8 cases/model in LP/HP; denoise
worst absolute error about 4.291e-6. Production USB LP/HP 105-second runs passed,
3345/3359 packets, CRC0, no pipeline errors. See tools/aot/denoise-validation.md.

Battery build was flashed and reported roughly 23.6-24 days at 11-12% busy:
464 USB packets, CRC0 in 16.7 seconds. Target, probe and JS110 then disappeared
together from USB. No sensor/model errors before disconnection. The planned
65-second test did not finish. Subsequent comment-only rebuild was not flashed.
Full rolling-window and LP/HP/mode-toggle hardware checks remain pending.
Added CPU duty histories use task-stack space; stack high-water validation is pending.

Local host validation: 11 C/C++ tests with ASan/UBSan, 4 power-report tests,
7 sleep layout/negative-link checks pass. AP510/AP330/AP510B builds pass.
Builds are not hardware acceptance. Proper AP330 Rev2 and AP510 sensor tests,
plus owner real-app acceptance, remain outstanding.

## Power evidence

All values are MCU-rail measurements, not entire EVB or sensor supply power.
See tools/bench/results/lp-power-20260909:
- RESULTS.md: repeated AOT helper windows, including input copy/loop overhead;
  all 15 output checks passed. Gate-only 100 us deglitching is documented.
- sleep-bank-sweep/README.md: seven controlled bank cases with post-capture
  bank states, BLE ENABLE latch/pad low, sticky CORESLEEP, no recorded wake.
  TCM160/384/768 with SRAM0: 0.995082/1.036714/1.106174 mW.
  TCM160 with SRAM0/1/2/3: 0.995082/1.155012/1.316267/1.474130 mW.
  Full-demo-capacity TCM768/SRAM1: 1.268080 mW.
- Earlier SLEEP-*.md records distinguish exploratory failures from qualified
  normal-sleep observations. No deep-sleep or cable-leakage qualification.

Streaming captures: tools/bench/results/active-recheck-20260909/README.md.
LP AI-on: 10.5127 and 10.5221 mW; AI-off: 10.4408 mW. All three capture checks
passed. The difference is not isolated inference power. Streaming keeps clocks,
transport and idle overhead active; do not bill that baseline only during busy time.

docs/full-demo-memory.md records static placement: DTCM276.12 KiB, shared
SRAM110.55 KiB and the pre-projection MRAM image. The application requires the
larger TCM pair but fits one shared-SRAM group and one MRAM bank. Sleep helpers
retain that capacity without running the application. Do not change production
bank policy based only on static fit; dynamic peaks and wake behavior need tests.

## Hardware and execution gotchas

AP510B probe1160002954; device AP510NFA-CBR. JS110004204 on positive MCU rail.
GP0/J8-1 to JS110 IN0; J8-14 GND to JS110 GND. Logic reference 1.8 V, no JS110
outputs/+5V connected. Sensor wiring and physical connections are owner-controlled.
Fixture was disconnected at the last check; do not assume an installed image.

Use NSX flash/reset/view or HPX wrappers. No raw J-Link fallback for empty SWO.
Quiet capture uses HPX SWPOI reset; debug snapshots only after recording.
JS110 range=off previously caused zero current and failed target attachment.
Enable auto range explicitly when approved; --open restore alone preserves off.
No debug/SWO viewer during analog windows.

Build one board at a time through NSX. Direct CMake reuse after changing boards
can pick up the other board's generated module list. End with AP510B.
NSX may remove nsx-gpio/nsx-interrupt/nsx-timer ignore entries; restore that
mechanical diff. Never stage modules/helia-rt, the optional external reference.

Paired UI: tileio/.claude/worktrees/vitals-dashboard-labels,
branch codex/vitals-dashboard-labels. Its root HANDOFF owns dashboard status.
Next: inspect local commits, complete remaining hardware checks, obtain approval
before merging or releasing either repo. Draft PR publication is complete.
