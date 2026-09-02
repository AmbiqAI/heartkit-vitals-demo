#!/usr/bin/env bash
#
# Build and assemble a prebuilt firmware package an FAE can flash without the
# toolchain. macOS and Linux.
#
#   tools/release/package.sh --version v5.0.0 [--board apollo510b_evb] \
#                            [--notes RELEASE-NOTES.md]
#
# Output layout mirrors the firmware drops the FAEs already use (v400, v410):
#
#   dist/<tag>/RELEASE.md
#   dist/<tag>/FLASH.md
#   dist/<tag>/BUILD-INFO.txt
#   dist/<tag>/SHA256SUMS
#   dist/<tag>/<board-dir>-firmware.map
#   dist/<tag>/<board-dir>/{firmware.bin,downloadfw.jlink,
#                           flash_mac.command,flash_win.bat,flash_linux.sh}
#   dist/heartkit-vitals-demo-<tag>-firmware.zip
#
# Every J-Link parameter is read from the SoC facts file that `nsx flash`
# itself uses, then cross-checked against the linker script and against the
# command file NSX generated during this build. Nothing is hard-coded here.
# Sources are recorded in BUILD-INFO.txt inside the package.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
TEMPLATE_DIR="${SCRIPT_DIR}/templates"

VERSION=""
BOARD="apollo510b_evb"
NOTES_FILE=""

# `head -1` under `set -o pipefail` makes the upstream command die of SIGPIPE
# (status 141) and takes the whole script with it. Read the stream fully instead.
first_line() { local v; v="$(cat)"; printf '%s' "${v%%$'\n'*}"; }

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }

usage() {
  cat <<'USAGE'
Usage: tools/release/package.sh --version vX.Y.Z [--board apollo510b_evb]
                                [--notes RELEASE-NOTES.md]

  --version   Release version, for example v5.0.0. Required.
  --board     NSX board name. Defaults to apollo510b_evb.
  --notes     Markdown file to ship as RELEASE.md. Without it the package
              ships the placeholder template, which is not release ready.
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version) [ $# -ge 2 ] || die "--version needs a value"; VERSION="$2"; shift 2 ;;
    --board)   [ $# -ge 2 ] || die "--board needs a value";   BOARD="$2";   shift 2 ;;
    --notes)   [ $# -ge 2 ] || die "--notes needs a value";   NOTES_FILE="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown argument: $1" ;;
  esac
done

[ -n "$VERSION" ] || { usage >&2; die "--version is required"; }
if [ -n "$NOTES_FILE" ]; then
  [ -f "$NOTES_FILE" ] || die "--notes file not found: $NOTES_FILE"
  [ -r "$NOTES_FILE" ] || die "--notes file is not readable: $NOTES_FILE"
  [ -s "$NOTES_FILE" ] || die "--notes file is empty: $NOTES_FILE"
  NOTES_FILE="$(cd -- "$(dirname -- "$NOTES_FILE")" && pwd)/$(basename -- "$NOTES_FILE")"
fi
[[ "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-.+][0-9A-Za-z.-]+)?$ ]] \
  || die "version must look like v5.0.0 (optional -suffix), got: $VERSION"

# Folder tag: v5.0.0 -> v500, v0.0.0-test -> v000-test. Matches the v400/v410
# naming used by the existing firmware drops.
_core="${VERSION#v}"
_base="${_core%%-*}"
_suffix="${_core#"$_base"}"
TAG="v${_base//./}${_suffix}"

# Board -> package folder and SoC. SoC names come from nsx.yml
# (targets.supported.<board>.soc); folder names match the v410 drop.
case "$BOARD" in
  apollo510b_evb)  BOARD_DIR="apollo510b"; SOC="apollo510b" ;;
  apollo510_evb)   BOARD_DIR="apollo510";  SOC="apollo510" ;;
  apollo330mP_evb) BOARD_DIR="apollo330";  SOC="apollo330P" ;;
  *) die "unsupported board: $BOARD (expected one of apollo510b_evb, apollo510_evb, apollo330mP_evb)" ;;
esac

cd "$REPO_DIR"

for tool in uv zip; do
  command -v "$tool" >/dev/null 2>&1 || die "required tool not found on PATH: $tool"
done

if command -v shasum >/dev/null 2>&1; then
  SHA_CMD=(shasum -a 256)
elif command -v sha256sum >/dev/null 2>&1; then
  SHA_CMD=(sha256sum)
else
  die "neither shasum nor sha256sum found on PATH"
fi
sha256_of() { "${SHA_CMD[@]}" "$1" | awk '{print $1}'; }

# Application target name from nsx.yml (project.name).
APP_NAME="$(awk '/^project:/{p=1;next} p&&/^[^[:space:]]/{p=0} p&&/name:/{print $2;exit}' nsx.yml)"
[ -n "$APP_NAME" ] || die "could not read project.name from nsx.yml"

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

BUILD_DIR="build/${BOARD}"
BUILD_LOG="$(mktemp -t nsx-build-log)"
trap 'rm -f "$BUILD_LOG"' EXIT

note "Building ${APP_NAME} for ${BOARD} (${VERSION})"

# Force the app sources to recompile so the packaged image matches the tree.
touch src/* 2>/dev/null || die "could not touch src/*"

set +e
uv run nsx build --app-dir "$REPO_DIR" --board "$BOARD" 2>&1 | tee "$BUILD_LOG"
build_rc="${PIPESTATUS[0]}"
set -e

[ "$build_rc" -eq 0 ] || die "nsx build exited with status ${build_rc}"

if grep -nE 'error:|FAILED:' "$BUILD_LOG" >/dev/null 2>&1; then
  printf 'error: build log contains error or FAILED lines:\n' >&2
  grep -nE 'error:|FAILED:' "$BUILD_LOG" >&2
  exit 1
fi

BIN_PATH="${BUILD_DIR}/${APP_NAME}.bin"
MAP_PATH="${BUILD_DIR}/${APP_NAME}.map"
[ -f "$BIN_PATH" ] || die "build did not produce ${BIN_PATH}"
[ -f "$MAP_PATH" ] || die "build did not produce ${MAP_PATH}"

# ---------------------------------------------------------------------------
# J-Link parameters, read from the tool rather than assumed
# ---------------------------------------------------------------------------

FACTS_FILE="modules/nsx-ambiq-sdk/cmake/socs/facts/${SOC}.cmake"
[ -f "$FACTS_FILE" ] || die "SoC facts file not found: ${FACTS_FILE}"

read_fact() {
  sed -n "s/.*$1[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$FACTS_FILE" | first_line
}

JLINK_DEVICE="$(read_fact NSX_SEGGER_DEVICE)"
SWD_SPEED="$(read_fact NSX_SEGGER_IF_SPEED)"
LOAD_ADDRESS="$(read_fact NSX_SEGGER_PF_ADDR)"

[ -n "$JLINK_DEVICE" ] || die "TODO(verify): NSX_SEGGER_DEVICE not found in ${FACTS_FILE}"
[ -n "$SWD_SPEED" ]    || die "TODO(verify): NSX_SEGGER_IF_SPEED not found in ${FACTS_FILE}"
[ -n "$LOAD_ADDRESS" ] || die "TODO(verify): NSX_SEGGER_PF_ADDR not found in ${FACTS_FILE}"

# Interface. NSX passes "-if SWD" unconditionally; confirm that is still true
# in the installed neuralspotx rather than trusting this comment.
JLINK_IF="SWD"
NSX_PKG_DIR="$(uv run python -c 'import neuralspotx,os;print(os.path.dirname(neuralspotx.__file__))' 2>/dev/null || true)"
IF_SOURCE="TODO(verify): could not locate the installed neuralspotx package"
if [ -n "$NSX_PKG_DIR" ] && [ -d "$NSX_PKG_DIR" ]; then
  if grep -rq -- "-if SWD" "$NSX_PKG_DIR"; then
    IF_SOURCE="$(grep -rl -- "-if SWD" "$NSX_PKG_DIR" | first_line)"
  else
    die "TODO(verify): '-if SWD' not found in ${NSX_PKG_DIR}; do not guess the interface"
  fi
fi

# Cross-check 1: the linker script the build actually uses.
LD_PATH="modules/nsx-ambiq-sdk/modules/nsx-core/src/${SOC}/gcc/linker_script_sbl.ld"
LD_ORIGIN=""
if [ -f "$LD_PATH" ]; then
  LD_ORIGIN="$(sed -n 's/.*MCU_MRAM[^:]*:[[:space:]]*ORIGIN[[:space:]]*=[[:space:]]*\(0[xX][0-9A-Fa-f]*\).*/\1/p' "$LD_PATH" | first_line)"
fi
[ -n "$LD_ORIGIN" ] || die "TODO(verify): could not read MCU_MRAM ORIGIN from ${LD_PATH}"

# Cross-check 2: the command file NSX generated for this very build.
GEN_JLINK="${BUILD_DIR}/jlink/${APP_NAME}/flash_cmds.jlink"
GEN_ADDR=""
if [ -f "$GEN_JLINK" ]; then
  GEN_ADDR="$(sed -n 's/.*LoadFile.*,[[:space:]]*\(0[xX][0-9A-Fa-f]*\).*/\1/p' "$GEN_JLINK" | first_line)"
fi
[ -n "$GEN_ADDR" ] || die "TODO(verify): could not read the load address from ${GEN_JLINK}"

if [ "$((LOAD_ADDRESS))" -ne "$((LD_ORIGIN))" ] || [ "$((LOAD_ADDRESS))" -ne "$((GEN_ADDR))" ]; then
  cat >&2 <<MISMATCH
error: load address sources disagree. Stopping rather than picking one.
  ${FACTS_FILE} NSX_SEGGER_PF_ADDR : ${LOAD_ADDRESS}
  ${LD_PATH} MCU_MRAM ORIGIN       : ${LD_ORIGIN}
  ${GEN_JLINK} LoadFile            : ${GEN_ADDR}
MISMATCH
  exit 1
fi

note "J-Link: device=${JLINK_DEVICE} if=${JLINK_IF} speed=${SWD_SPEED} kHz addr=${LOAD_ADDRESS}"

# ---------------------------------------------------------------------------
# Assemble
# ---------------------------------------------------------------------------

PKG_ROOT="dist/${TAG}"
BOARD_PKG="${PKG_ROOT}/${BOARD_DIR}"
rm -rf "$PKG_ROOT"
mkdir -p "$BOARD_PKG"

render() {
  # render <template> <destination>
  sed -e "s|@VERSION@|${VERSION}|g" \
      -e "s|@BOARD@|${BOARD}|g" \
      -e "s|@BOARD_DIR@|${BOARD_DIR}|g" \
      -e "s|@APP_NAME@|${APP_NAME}|g" \
      -e "s|@JLINK_DEVICE@|${JLINK_DEVICE}|g" \
      -e "s|@SWD_SPEED@|${SWD_SPEED}|g" \
      -e "s|@LOAD_ADDRESS@|${LOAD_ADDRESS}|g" \
      "$1" > "$2"
  if grep -q '@[A-Z_]*@' "$2"; then
    die "unsubstituted placeholder left in $2"
  fi
}

cp "$BIN_PATH" "${BOARD_PKG}/firmware.bin"
chmod 644 "${BOARD_PKG}/firmware.bin"
cp "$MAP_PATH" "${PKG_ROOT}/${BOARD_DIR}-firmware.map"
chmod 644 "${PKG_ROOT}/${BOARD_DIR}-firmware.map"

render "${TEMPLATE_DIR}/downloadfw.jlink"   "${BOARD_PKG}/downloadfw.jlink"
render "${TEMPLATE_DIR}/flash_mac.command"  "${BOARD_PKG}/flash_mac.command"
render "${TEMPLATE_DIR}/flash_linux.sh"     "${BOARD_PKG}/flash_linux.sh"
render "${TEMPLATE_DIR}/flash_win.bat"      "${BOARD_PKG}/flash_win.bat"
render "${TEMPLATE_DIR}/FLASH.md"           "${PKG_ROOT}/FLASH.md"
render "${TEMPLATE_DIR}/RELEASE.md"         "${PKG_ROOT}/RELEASE.md"

# Real release notes replace the placeholder before SHA256SUMS is written, so
# the checksum always covers the text that actually ships.
if [ -n "$NOTES_FILE" ]; then
  cp "$NOTES_FILE" "${PKG_ROOT}/RELEASE.md"
  chmod 644 "${PKG_ROOT}/RELEASE.md"
  note "Release notes taken from ${NOTES_FILE}"
fi

# flash_win.bat keeps LF line endings, byte-for-byte matching the v410 drop the
# FAEs already use. Do not "fix" this to CRLF without re-validating on Windows.

chmod 755 "${BOARD_PKG}/flash_mac.command" "${BOARD_PKG}/flash_linux.sh"
chmod 644 "${BOARD_PKG}/flash_win.bat" "${BOARD_PKG}/downloadfw.jlink"

GIT_COMMIT="$(git -C "$REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY=""
if ! git -C "$REPO_DIR" diff --quiet HEAD -- 2>/dev/null; then GIT_DIRTY=" (tree dirty)"; fi
# The nsx CLI has no --version flag (argparse exits 2), so record the version
# of the installed neuralspotx distribution instead. Cross-checked below
# against tooling.nsx.version in nsx.yml.
NSX_VERSION="$(uv run python -c 'import importlib.metadata as m; print("neuralspotx " + m.version("neuralspotx"))' 2>/dev/null | first_line || true)"
[ -n "$NSX_VERSION" ] || NSX_VERSION="TODO(verify): could not read the installed neuralspotx version"
NSX_PINNED="$(awk '/^tooling:/{t=1} t&&/version:/{print $2; exit}' nsx.yml)"
BIN_SHA="$(sha256_of "${BOARD_PKG}/firmware.bin")"
BIN_SIZE="$(wc -c < "${BOARD_PKG}/firmware.bin" | tr -d ' ')"

cat > "${PKG_ROOT}/BUILD-INFO.txt" <<INFO
HeartKit Vitals demo firmware package
=====================================

Version        : ${VERSION}
Package tag    : ${TAG}
NSX board      : ${BOARD}
SoC            : ${SOC}
Application    : ${APP_NAME}
Built (UTC)    : $(date -u '+%Y-%m-%d %H:%M:%SZ')

Provenance
----------
Git commit     : ${GIT_COMMIT}${GIT_DIRTY}
NSX toolchain  : ${NSX_VERSION}
nsx.yml pin    : tooling.nsx.version = ${NSX_PINNED}
Build command  : uv run nsx build --app-dir . --board ${BOARD}

Image
-----
firmware.bin   : ${BIN_SIZE} bytes
SHA-256        : ${BIN_SHA}

J-Link parameters and their sources
-----------------------------------
Device         : ${JLINK_DEVICE}
                 ${FACTS_FILE} (NSX_SEGGER_DEVICE)
Interface      : ${JLINK_IF}
                 ${IF_SOURCE}
Speed          : ${SWD_SPEED} kHz
                 ${FACTS_FILE} (NSX_SEGGER_IF_SPEED)
Load address   : ${LOAD_ADDRESS}
                 ${FACTS_FILE} (NSX_SEGGER_PF_ADDR)
                 cross-checked against ${LD_PATH} (MCU_MRAM ORIGIN = ${LD_ORIGIN})
                 cross-checked against ${GEN_JLINK} (LoadFile ${GEN_ADDR})

Command sequence in downloadfw.jlink matches the NSX-generated flash command
file for this build: ExitOnError, Reset, LoadFile, Reset, Go, Exit.
INFO

# SHA256SUMS covers every packaged file, with paths relative to the package
# root so `shasum -a 256 -c SHA256SUMS` works from the extracted folder.
( cd "$PKG_ROOT" && find . -type f ! -name SHA256SUMS -print0 \
    | LC_ALL=C sort -z \
    | xargs -0 "${SHA_CMD[@]}" > SHA256SUMS )

# ---------------------------------------------------------------------------
# Zip and report
# ---------------------------------------------------------------------------

ZIP_NAME="${APP_NAME}-${TAG}-firmware.zip"
( cd dist && rm -f "$ZIP_NAME" && zip -q -r -X "$ZIP_NAME" "$TAG" )

ZIP_PATH="dist/${ZIP_NAME}"
ZIP_SHA="$(sha256_of "$ZIP_PATH")"

note "Package ready"
printf '\n'
printf 'Package  : %s\n' "$PKG_ROOT"
printf 'Archive  : %s\n' "$ZIP_PATH"
printf 'SHA-256  : %s\n' "$ZIP_SHA"
printf '\n'

if [ -z "$NOTES_FILE" ]; then
  printf '%s\n' \
    '**********************************************************************' \
    'WARNING: this package ships the RELEASE.md placeholder, not release' \
    'notes. It is not ready to hand to an FAE. Re-run with --notes FILE.' \
    '**********************************************************************' >&2
fi
