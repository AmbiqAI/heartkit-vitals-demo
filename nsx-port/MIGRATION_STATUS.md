# neuralSPOT → neuralSPOT-X (NSX) migration status

Handoff note for whoever picks this up next (human or agent). Hardware
(AP510/AP510B EVB + AS7058 sensor board) is moving to a remote dev machine;
this doc captures where things stand so setup/context isn't lost.

## TL;DR

The NSX port (`nsx-port/`) is **feature-complete and hardware-validated** on
all 3 target boards, with USB + BLE TileIO streaming, canned patient
playback, and full DSP/AI ECG+PPG pipelines. Several real bugs were found
and fixed upstream (not just worked around) during bring-up — see
"Upstream fixes" below. The branch (`agents/refactor-to-neuralspot-x`) is
being pushed as a PR against `main` now. One task is still open: a
systematic review of ringbuffer/task ownership for race conditions (see
"Open work" below).

## What works (hardware-verified on apollo510b_evb)

- AS7058 sensor bring-up: dual-wavelength Red+IR PPG, ECG, SpO2, correct
  AGC scaling.
- Full DSP/AI pipeline: ECG denoise (DSP biquad + AI model), segmentation
  (DSP peak-finder + AI model), arrhythmia classification, PPG metrics.
- TileIO streaming over **USB** (WebUSB) and **BLE** (GATT) simultaneously,
  same data fan-out from one queue.
- Host→device UIO commands (input source, denoise/seg/arrhythmia mode,
  noise levels, speed mode) — confirmed working end-to-end from the real
  production web app, not just test tooling.
- Canned patient-data playback (non-live input sources substitute
  pre-recorded ECG/PPG stimulus through the same pipeline).
- All 3 boards build clean: `apollo510_evb`, `apollo510b_evb` (BLE-capable,
  primary hardware target), `apollo330mP_evb`.

## Upstream fixes (real bugs, not local workarounds)

These were root-caused and fixed in the actual upstream repos, then
re-locked into `nsx-port/nsx.lock`/`nsx.yml`. If you ever see these
classes of symptoms again elsewhere, these are the reference fixes:

1. **`AmbiqAI/nsx-ambiq-sdk` PR #39** (merged `201aae7`) — `nsx-usb`'s
   vendor send wrapped every USB chunk in a *global* IRQ mask
   (`am_hal_interrupt_master_disable/enable`), which could starve the
   AS7058's edge-triggered INT line long enough to permanently kill its
   interrupt generation. Replaced with a single-core task/ISR lock flag.

2. **`AmbiqAI/nsx-ambiq-sdk` PR #40** (merged `a9acf52`) — `nsx-usb`'s
   TinyUSB callback overrides (`tud_vendor_control_xfer_cb`,
   `tud_mount_cb`, etc., in `nsx_usb_overrides.c`) were silently linking as
   TinyUSB's *weak* no-op stubs instead of our strong definitions — being
   in the same static archive as a weak stub does **not** guarantee GNU ld
   picks the strong one. Fixed with `--whole-archive`/`--no-whole-archive`.
   This was the actual reason the browser's standard WebUSB connect
   sequence (`SET_CONTROL_LINE_STATE` control transfer only, no bulk data)
   silently stalled — the real web app never needed a firmware patch on
   *its* side, ours was broken.

3. **`AmbiqAI/helia-dsp` PR #4** (merged `9c18599`) — on Cortex-M55, the
   compiler auto-defines `__ARM_FEATURE_MVE`, which silently switched
   CMSIS-DSP's `arm_biquad_cascade_df1_f32` (and friends) to an
   MVE-vectorized code path expecting a totally different coefficient
   layout than what this app / `nsx-physiokit` use — producing
   degenerate/all-zero filter output with **no build error**. This was the
   actual cause of the ECG "denoised" channel flatlining whenever
   `denoiseMode != Off` (which includes the AI default). Fixed by forcing
   `ARM_MATH_AUTOVECTORIZE` (now correctly `PUBLIC`, was `PRIVATE`) for NSX
   board builds. Also disabled several costly CI test-matrix workflows
   inherited unchanged from upstream CMSIS-DSP (15+ min/run) per repo
   owner request — not relevant to this fork's actual usage.

4. **`AmbiqAI/tileio` PR #18** (merged `396305b`) + **`AmbiqAI/nsx-tileio`
   PR #2** (merged `2f89e61`) — the production web app's `setUioState()`
   was chunking every host→device write into 62-byte payloads with a
   2-byte legacy "NS frame header" per 64-byte USB transfer (leftover from
   an older neuralSPOT sample that multiplexed 2 message types over one
   endpoint). This firmware's `nsx-tileio-usb` never stripped that header,
   so every real web-app UIO write corrupted the packet framing and was
   silently dropped — **this was the actual cause of "selections in the
   web app UI don't take effect on the device."** Fixed at the source in
   both directions (web app now sends one raw unframed write, matching how
   reads already worked) rather than teaching firmware to parse the legacy
   framing — also removes ~25% wire overhead.

**Pattern across all 4**: every one of these was a genuine bug in shared
NSX/CMSIS infrastructure that would bite *any* app using these modules on
this SoC, not something specific to this demo. Worth remembering next time
something "should work per code trace" but doesn't on real hardware —
check for weak-symbol linking, MVE auto-detection, and legacy protocol
baggage before assuming app-level logic is at fault.

## Open work

- **Ringbuffer/task ownership race-condition review** — not yet done.
  Candidate areas to audit in `src/main.cc`:
  - `g_uio_pending`/`g_flush_req_{ecg,ppg,cpu}` flag-based cross-task
    signaling (ISR → task, task → task) — currently correct as far as
    hardware testing has shown, but the pattern (raise flag, consumer
    polls and clears) is ad-hoc and repeated 4+ times; a shared primitive
    (or FreeRTOS event group) might be more robust and easier to reason
    about than one-off volatile flags each.
  - `ringbuffer.c`'s single-producer/single-consumer contract is
    documented but not enforced — worth double-checking every ringbuffer's
    actual producer/consumer task assignment still matches its comment
    now that BLE fan-out adds a second consumer path for the same TX data
    (queue-based, should be fine, but wasn't specifically re-audited after
    BLE was added).
  - No known hardware-observed race failures — this is a
    robustness/hardening pass, not a bug hunt for a known symptom.

## Hardware setup (for the remote machine)

- Board: Apollo510B EVB (`apollo510b_evb`) with AS7058 click sensor board
  attached — this is the only board with BLE (EM9305 radio); the other 2
  board targets (`apollo510_evb`, `apollo330mP_evb`) build but haven't been
  hardware-validated with the same depth (they were validated earlier in
  the port, before the 4 upstream fixes above — worth a quick re-flash +
  smoke test on whichever of those you still have access to, but the
  primary target and the one with all fixes verified live is
  `apollo510b_evb`).
- J-Link SWD for flash + SWO console (`nsx flash --app-dir nsx-port --board
  apollo510b_evb`, `nsx view --app-dir nsx-port --board apollo510b_evb`).
  See `nsx-port/README.md` for full build/flash/view command reference —
  note its "known SWO limitation" callout is **stale**; SWO/RTT console
  output worked reliably and extensively throughout this session's
  hardware bring-up (used constantly for live debugging). That note was
  from an earlier environment and should probably just be deleted next
  time someone touches that README.
- USB: device enumerates as `VID=0xCAFE PID=0x0001`, needs the target's
  own USB cable connected to the host (separate from the J-Link debug USB)
  for WebUSB/TileIO traffic.
- `nsx-port/tools/tileio_usb_test.py` — pyusb-based standalone verification
  tool (no browser needed). Good first smoke test after moving hardware:
  `python3 nsx-port/tools/tileio_usb_test.py --duration 5` should report
  `bad=0` and show ECG/PPG/CPU signal+metrics packets streaming.
- Real web app: `AmbiqAI/tileio` (separate repo, not part of this one) —
  make sure whatever checkout is used on the remote machine has pulled the
  merged `fix/uio-write-drop-ns-frame-chunking` change (now on `main`,
  commit `396305b`) or UIO writes will silently fail exactly like the bug
  described above.

## Useful context for continuing

- All upstream module pins are explicit in `nsx-port/nsx.yml`'s
  `module_registry` section (added several overrides this session since
  some modules, e.g. `helia-dsp` and `nsx-tileio`, aren't pinned by nsx's
  own bundled default registry in a way `nsx update` alone can move past a
  fixed-commit constraint — if a similar situation comes up again, add an
  explicit `module_registry.projects.<name>` + `.modules.<name>` entry
  pointing at the exact commit, then `nsx lock`).
- `modules/` is gitignored and regenerated by `nsx configure`/`sync`/`lock`
  — **any direct edits there will be silently wiped** the next time one of
  those commands runs. This bit me more than once this session. If you
  need to patch a vendored module, do it in a fresh clone of the actual
  upstream repo, verify there, then copy into `modules/` for local
  testing, and always push a real PR upstream before considering the fix
  durable — `nsx.lock`/`nsx.yml` are the only source of truth for what
  actually ships.
- `nsx-port/tools/tileio_usb_test.py`'s module docstring documents the
  current (correct) TileIO USB wire protocol in detail — read that first
  if debugging USB framing issues again.
