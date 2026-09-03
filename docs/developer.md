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

## Continuous Integration

`.github/workflows/ci.yml` runs on every pull request and on pushes to `main`.
It uses `scripts/ci-local.sh` as its entry point, so a local green run and a CI
green run mean the same thing.

| Job | Runner | What it runs |
| --- | --- | --- |
| `host` | `ubuntu-latest` | `scripts/ci-local.sh tests`, `shellcheck scripts/*.sh`, `uv run nsx lock --app-dir . --check` |
| `firmware` | `ubuntu-latest`, matrix over `apollo510b_evb`, `apollo510_evb`, `apollo330mP_evb` | frozen module sync, `scripts/ci-local.sh frozen`, per-board `nsx configure --frozen` and `nsx build`, uploads `firmware.bin` per board |
| `notices` | `macos-latest` | frozen module sync, then `tools/release/gen_third_party_notices.py --check` |

`uv run nsx lock --app-dir . --check` is not a full lockfile gate on the
runner. The `host` job has no module credentials, so the check falls back to
the lock closure rather than re-resolving every module source; it catches a
manifest edit that was never locked, and it does not catch upstream drift. See
[#50](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/50).

`ci.yml` sets `CI_STRICT=1` for its `scripts/ci-local.sh` steps; the release
job does not run that script. `scripts/ci-local.sh` skips a check it
cannot run locally, for example when `uv` is missing or the vendored module
paths are not materialised; under `CI_STRICT=1` each of those paths fails
instead, so a gate step cannot pass having run nothing.

The `notices` job needs macOS because the generator converts the AmbiqSuite
agreement from RTF with `textutil`, which only macOS provides. The generator
fails closed when no converter is present, so this check cannot move to Linux
until a portable converter or a committed text copy exists.

There is no shared embedded build image, so both firmware paths download the
Arm GNU Toolchain 15.2.Rel1 from the Arm developer site and verify the archive
against the SHA-256 that Arm publishes next to it. Every `uses:` is pinned by
commit SHA.

### Owner action: the `NSX_MODULE_TOKEN` secret

The module closure in `nsx.lock` is vendored from private AmbiqAI
repositories, which a hosted runner cannot read with the default job token. Add
a repository secret named `NSX_MODULE_TOKEN` holding a token with read access
to those repositories. Until it exists, the `firmware` and `notices` jobs fail
with `NSX_MODULE_TOKEN secret is not set`. They fail rather than skip on
purpose: a skipped build gate reads as a pass on the pull request.

### Release workflow

`.github/workflows/release.yml` runs on a `v[0-9]*` tag push, and can also be
started by hand with `workflow_dispatch` and a version input. The tag must
already exist either way: the job fails if `refs/tags/<version>` does not
resolve, and `gh release create --verify-tag` stops the release step from
creating a tag as a side effect. The job only ever writes to a draft release;
if a release for that version is already published it fails rather than
replacing live assets. It runs
`tools/release/package.sh` for the three release boards on a macOS runner,
uploads the zips as workflow artifacts, and attaches them to a **draft**
GitHub release. Publication stays manual, because the hardware validation gate
is recorded by hand in the release notes first. `HKV_V410_REF_DIR` is not set
in CI, so the flash helper comparison is recorded as not compared; run the
comparison locally when it matters.

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
