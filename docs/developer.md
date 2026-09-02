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

## Release Packaging

`tools/release/package.sh` assembles a prebuilt firmware drop and, as part of
its release gate, byte-compares the rendered flash helpers
(`downloadfw.jlink`, `flash_mac.command`, `flash_win.bat`, `flash_linux.sh`)
against the field-proven v4.1.0 drop. A difference is a loud warning, not a
build failure.

The script carries no default path for that v4.1.0 reference. To enable the
comparison, set `HKV_V410_REF_DIR` to a local mount of the v4.1.0 firmware
folder from the team OneDrive (`AITG - Documents` library, under
`Demos/vital-sign-monitoring/firmware/v410`), for example:

```bash
export HKV_V410_REF_DIR="$HOME/Library/CloudStorage/OneDrive-AmbiqMicroInc/AITG - Documents/Demos/vital-sign-monitoring/firmware/v410"
```

Without `HKV_V410_REF_DIR` set, or if it points at a folder that is not
there, packaging still succeeds; the log and `BUILD-INFO.txt` record the
comparison as `not compared (HKV_V410_REF_DIR not set)`.

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
