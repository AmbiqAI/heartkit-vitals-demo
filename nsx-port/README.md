# heartkit-vitals-demo — NSX port (in progress)

This is an in-progress port of `heartkit-vitals-demo` from legacy
neuralSPOT (GNU Make) to [neuralSPOT-X](https://github.com/AmbiqAI/neuralspotx)
(NSX/CMake). It lives in this `nsx-port/` subfolder so the legacy Make build
at the repo root keeps working, untouched, until the port reaches full
parity and hardware validation. See the top-level port plan for phase
status.

## Supported boards

This app targets, and is validated on, three Apollo5 EVB variants (all with
the AS7058 PPG/ECG click module over I2C):

- `apollo510_evb`
- `apollo510b_evb` — **current primary development board** (has a dedicated
  BLE radio; default board in `nsx.yml`/`CMakeLists.txt`)
- `apollo330mP_evb`

All three build cleanly from the same source tree; board selection is via
`--board` (see below). No board-specific `#ifdef`s exist in the app sources
today (`AS7058_BOARD_PROFILE`/`AS7058_APP_PROFILE` in `src/constants.h`
select transport/profile, not board/SoC).

## Build

```bash
nsx configure --app-dir .                      # uses default board (apollo510b_evb)
nsx configure --app-dir . --board apollo510_evb   # or override per-board
nsx build --app-dir .
nsx flash --app-dir .
nsx view --app-dir .
```

`nsx view` opens the SEGGER SWO viewer with the board-appropriate reset
policy. **Known environment limitation**: in this sandbox, `nsx view`
produces no console output regardless of app/board (reproduced identically
with the already-proven `ppg-codec-demo` reference app) — this is a
tooling/environment issue, not a defect in the port. Clean build + flash/
reset over SWD is the practical acceptance signal here; re-verify SWO/RTT
console output on a machine with working trace capture.

## Phase 2 status: AS7058 sensor bring-up

`src/sensor.c` streams raw PPG1_SUB1 + ECG_SEQ1_SUB1 samples from the AS7058
into two ringbuffers; `src/main.c`'s `ReportTask` prints push/drop/ISR
counts once per second as the acceptance signal. `nsx-physiokit` (DSP) is
intentionally **not** yet added — see the port plan for the known
`helia-dsp`/`CMSISDSP` toolchain-flags packaging gap that must be resolved
before Phase 3/4 DSP integration.

## Layout

- `cmake/nsx/` — copied NSX CMake support (gitignored/regenerated).
- `modules/` — NSX module sources vendored per `nsx.lock` (gitignored).
- `boards/` — vendored board definitions for `apollo510_evb`,
  `apollo510b_evb`, `apollo330mP_evb`.
- `nsx.yml` / `nsx.lock` — app manifest / pinned module+board resolution.
- `src/` — ported application sources (sensor, ringbuffer, as7058 profiles,
  store, main).
