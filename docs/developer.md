# Developer Guide

## Prerequisites

- Python 3.11 or later with `uv`.
- Arm GNU Toolchain (`arm-none-eabi-gcc`).
- SEGGER J-Link software and a supported EVB for flashing or SWO output.

Install the Python environment once:

```bash
uv sync
```

## Configure and Build

NSX downloads the dependency closure recorded in `nsx.lock` during configure.
Generated dependencies live under `modules/`; do not edit or commit them.

```bash
uv run nsx configure --app-dir . --board apollo510_evb
uv run nsx build --app-dir . --board apollo510_evb
```

Replace `apollo510_evb` with `apollo510b_evb` or `apollo330mP_evb` as needed.
Build artifacts are written to `build/<board>/`.

## Flash and View

Connect the board debugger, power the board, then run:

```bash
uv run nsx flash --app-dir . --board apollo510b_evb
uv run nsx view --app-dir . --board apollo510b_evb
```

`nsx flash` erases, programs, verifies, resets, and starts the target. If
J-Link cannot attach, check board power, reset state, and the SWD/debug USB
connection before retrying.

## Validation

Check that the lockfile still matches the manifest:

```bash
uv run nsx lock --app-dir . --check
```

After flashing a board with the USB data connection attached, run the TileIO
smoke test:

```bash
python3 tools/tileio_usb_test.py --duration 5
```

The test requires `pyusb` and should report `bad=0` while ECG, PPG, CPU, and
metric packets are received.

## Dependency Updates

Update `nsx.yml` when adding or changing an NSX module, then regenerate the
lockfile:

```bash
uv run nsx lock --app-dir .
```

Use pinned revisions for project-specific module overrides. Do not directly
edit `modules/` or `cmake/nsx/`; both are generated from the NSX manifest and
lockfile.

`src/generated/` is likewise generated, from the ams-OSRAM AS7058 GUI presets in
`assets/` via `tools/as7058_json_to_profile.py`; it carries no SPDX header
because it inherits the terms of its ams-OSRAM source. See `assets/README.md`
for asset provenance and `docs/as7058_profiles.md` for regeneration.

## Clean Working State

Remove only a target build directory:

```bash
uv run nsx clean --app-dir . --board apollo510b_evb --full
```

Return to a fresh dependency state:

```bash
uv run nsx clean --app-dir . --reset --force
```

The reset command removes `build/`, `.nsx/`, and generated dependencies. Run
`uv run nsx configure --app-dir . --board <board>` again before the next build.
