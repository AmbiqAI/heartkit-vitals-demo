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

### Transport counters

Capture the transport counters for three minutes with the TileIO dashboard
open and streaming, so the host is actually draining the queue:

```bash
python3 tools/bench/swo_capture.py 180 /tmp/bench-usb.log --app-dir . --board apollo510b_evb
python3 tools/bench/hkv_compare.py /tmp/bench-usb.log
```

The counters that matter are `ecg_retry`, `ecg_drop` and `stall` from the
`tiousb` line, and `qdrop` and `qdepth` from the `tio` line. `hkv_compare.py`
reports the `tiousb` counters as deltas over the whole capture and over its
second half, `qdrop` as a whole-capture delta, and `qdepth` as a distribution,
because the firmware counters are cumulative and a drop that only starts once
buffers fill does not show in a whole-window average.

Pass in this steady-state case, with the dashboard visible and the host
draining, is zero drops and zero stalls in both halves of the capture. Any
non-zero drop or stall here is a transport regression, tracked on #56. Under
an induced stall, counted drops past the hold watermark are designed
behavior; see `docs/design/streaming-pipeline.md` section 7 for the full
acceptance matrix.

## Continuous Integration

`.github/workflows/ci.yml` runs on every pull request and on pushes to `main`.
It uses `scripts/ci-local.sh` as its entry point, so a local green run and a CI
green run mean the same thing.

| Job | Runner | What it runs |
| --- | --- | --- |
| `host` | `ubuntu-latest` | `scripts/ci-local.sh tests`, `shellcheck scripts/*.sh`, a lock-consistency check that compares the manifest hash only |
| `firmware` | `ubuntu-latest`, matrix over `apollo510b_evb`, `apollo510_evb`, `apollo330mP_evb` | frozen module sync, `scripts/ci-local.sh frozen`, per-board `nsx configure --frozen` and `nsx build`, uploads `firmware.bin` per board |
| `notices` | `macos-latest` | frozen module sync, then `tools/release/gen_third_party_notices.py --check` |

The `host` job runs a lock-consistency check, not a full lockfile gate. It
holds no module credentials on purpose, so it cannot re-resolve module sources.
It used to run `uv run nsx lock --app-dir . --check`, which printed
`fatal: could not read Username for 'https://github.com'`, fell back to the
closure recorded in `nsx.lock` and exited 0 anyway; a green step therefore
looked like a full gate. nsx has no offline resolve mode, so instead of relying
on that fallback the step now compares only what it can check without the
network: the manifest hash of `nsx.yml` against the hash recorded for every
target in `nsx.lock`, using nsx's own `hash_manifest`, which is the same hash
`nsx sync --frozen` enforces. A passing log has no `fatal:` lines.

What that check does and does not cover:

- It catches an `nsx.yml` edit that was never followed by `nsx lock`.
- It does not catch upstream drift, meaning a module repository moving ahead
  of the commit recorded in `nsx.lock`. That is caught by the credentialed
  `firmware` and `notices` jobs, which run `nsx sync --app-dir .` followed by
  `git diff --exit-code -- nsx.lock`, and locally by `uv run nsx lock --check`
  with module access.
- Because `hash_manifest` re-serializes the parsed YAML, a comment-only edit to
  `nsx.yml` does not change the hash and does not require a relock.

See [#50](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/50).

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

`BUILD-INFO.txt` also records the C compiler version and path, read from the
build tree's `CMakeFiles/<cmake-version>/CMakeCCompiler.cmake`, next to the
NSX toolchain line, so the drop carries the toolchain it was built with.
Packaging stops if that record is missing rather than shipping a placeholder.

## Release Gate

The gate runs by hand on hardware, on the packaged artifact rather than a
local build tree. Publication is manual for the same reason: the gate result
is written into the release notes before anything is published.

1. Flash the packaged binary through the helper shipped in its own board
   folder (`dist/<slug>/<board>/flash_mac.command` or the Windows or Linux
   equivalent). Flashing from the build tree instead does not test what
   ships.
2. Capture 80 seconds of telemetry and read the settled figures:

   ```bash
   python3 tools/bench/swo_capture.py 80 /tmp/gate.log --app-dir . --board apollo510b_evb
   python3 tools/bench/hkv_analyze.py /tmp/gate.log --label gate
   ```

3. Compare the settled `util_x100`, `batt_*_x100` and `avg_ips_x100` means
   against the expected LP figures tracked on #27. The uptime against wall
   clock ratio should sit near 1.0; a ratio far off means the capture or the
   timebase is wrong and the figures cannot be read.
4. Run the transport counter capture above.
5. Record the result in the release notes, then publish. Publication is
   manual until a reviewed helper lands. See #39.

An empty capture is a stop-and-report condition. See `tools/bench/README.md`.

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
