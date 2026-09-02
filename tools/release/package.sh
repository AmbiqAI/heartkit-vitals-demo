#!/usr/bin/env bash
#
# Build and assemble a prebuilt firmware package an FAE can flash without the
# toolchain. macOS and Linux.
#
#   tools/release/package.sh --version v5.0.0 \
#                            [--board apollo510b_evb]... \
#                            [--boards apollo510_evb,apollo330mP_evb] \
#                            [--notes RELEASE-NOTES.md] \
#                            [--validation-note '<board>=<text>']...
#
# Several boards go into one drop. Each board is built from its own removed and
# freshly configured --frozen build tree and lands in its own folder; the
# top-level files describe the whole drop. Re-running for another board ADDS to
# an existing dist/<tag>/ instead of replacing it, so the three v5.0.0 boards
# can be packaged in separate runs (see #33).
#
# Output layout mirrors the firmware drops the FAEs already use (v400, v410):
#
#   dist/<tag>/RELEASE.md
#   dist/<tag>/FLASH.md
#   dist/<tag>/BUILD-INFO.txt          (one section per board in the drop)
#   dist/<tag>/SHA256SUMS
#   dist/<tag>/<board-dir>-firmware.map
#   dist/<tag>/<board-dir>/{firmware.bin,downloadfw.jlink,
#                           flash_mac.command,flash_win.bat,flash_linux.sh}
#   dist/heartkit-vitals-demo-<tag>-<board-dir>-firmware.zip   (one per board)
#   dist/heartkit-vitals-demo-<tag>-firmware.zip               (whole drop,
#                                        only when the drop has >1 board)
#
# Naming is load bearing. heartkit-vitals-demo-v500-firmware.zip is the name
# already attached to the v5.0.0 GitHub release, and apollo510b/ and
# apollo510b-firmware.map are the names already published inside it, so none of
# them change here; the per-board zips are new names added alongside.
#
# Every J-Link parameter is read from the SoC facts file that `nsx flash`
# uses, then cross-checked against the parameters CMake actually resolved for
# the flash target, against the linker script, and against the command file
# NSX generated during this build. The facts file is not the whole story: a
# board may override the device in boards/<board>/debug.cmake, in which case
# the override must equal what CMake resolved and the helpers are rendered from
# the resolved value. Nothing is hard-coded here. Sources are recorded in
# BUILD-INFO.txt.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
TEMPLATE_DIR="${SCRIPT_DIR}/templates"

VERSION=""
NOTES_FILE=""
BOARDS=()
VALIDATION_BOARDS=()
VALIDATION_TEXTS=()

# Firmware sources are compared against this tag in BUILD-INFO (issue #33).
REF_TAG="v5.0.0"
REF_PATHS=(src config boards CMakeLists.txt nsx.yml nsx.lock)

# Default hardware validation statement for a board with no --validation-note.
# The v5.0.0 drop ships apollo510_evb and apollo330mP_evb without a hardware
# gate; the owner decision and the reasoning are on #33.
DEFAULT_VALIDATION_NOTE="not flashed on hardware in this release cycle"

# Field-proven v4.1.0 drop used for the byte comparison of the rendered
# helpers. Override with HKV_V410_REF_DIR when the share is mounted elsewhere.
V410_REF_DIR="${HKV_V410_REF_DIR:-/Users/adam.page/Library/CloudStorage/OneDrive-AmbiqMicroInc/AITG - Documents/Demos/vital-sign-monitoring/firmware/v410}"

# `head -1` under `set -o pipefail` makes the upstream command die of SIGPIPE
# (status 141) and takes the whole script with it. Read the stream fully instead.
first_line() { local v; v="$(cat)"; printf '%s' "${v%%$'\n'*}"; }

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }

usage() {
  cat <<'USAGE'
Usage: tools/release/package.sh --version vX.Y.Z [--board BOARD]...
                                [--boards BOARD,BOARD] [--notes RELEASE-NOTES.md]
                                [--validation-note 'BOARD=text']...

  --version          Release version, for example v5.0.0. Required.
  --board            NSX board name. Repeatable. Defaults to apollo510b_evb.
  --boards           Comma separated NSX board names, same effect as repeating
                     --board.
  --notes            Markdown file to ship as RELEASE.md. Without it the package
                     ships the placeholder template, which is not release ready.
  --validation-note  Hardware validation statement for one board, given as
                     'BOARD=text'. Repeatable. Boards without a note record
                     "not flashed on hardware in this release cycle".

Boards already present in dist/<tag>/ from an earlier run are kept; the
top-level files and the checksums are regenerated over everything present.
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version) [ $# -ge 2 ] || die "--version needs a value"; VERSION="$2"; shift 2 ;;
    --board)   [ $# -ge 2 ] || die "--board needs a value";   BOARDS+=("$2"); shift 2 ;;
    --boards)
      [ $# -ge 2 ] || die "--boards needs a value"
      _list="${2//,/ }"
      for _b in $_list; do [ -n "$_b" ] && BOARDS+=("$_b"); done
      shift 2 ;;
    --notes)   [ $# -ge 2 ] || die "--notes needs a value";   NOTES_FILE="$2"; shift 2 ;;
    --validation-note)
      [ $# -ge 2 ] || die "--validation-note needs a value"
      case "$2" in
        *=*) VALIDATION_BOARDS+=("${2%%=*}"); VALIDATION_TEXTS+=("${2#*=}") ;;
        *) die "--validation-note must look like BOARD=text, got: $2" ;;
      esac
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown argument: $1" ;;
  esac
done

[ "${#BOARDS[@]}" -gt 0 ] || BOARDS=("apollo510b_evb")

# De-duplicate the board list while keeping the order given.
_uniq=()
for _b in "${BOARDS[@]}"; do
  _seen=0
  for _u in "${_uniq[@]:-}"; do [ "$_u" = "$_b" ] && _seen=1; done
  [ "$_seen" -eq 0 ] && _uniq+=("$_b")
done
BOARDS=("${_uniq[@]}")

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
BOARD_DIR=""
SOC=""
board_facts() {
  case "$1" in
    apollo510b_evb)  BOARD_DIR="apollo510b"; SOC="apollo510b" ;;
    apollo510_evb)   BOARD_DIR="apollo510";  SOC="apollo510" ;;
    apollo330mP_evb) BOARD_DIR="apollo330";  SOC="apollo330P" ;;
    *) die "unsupported board: $1 (expected one of apollo510b_evb, apollo510_evb, apollo330mP_evb)" ;;
  esac
}

# Boards cleared for release packaging. apollo510_evb and apollo330mP_evb were
# added for the v5.0.0 drop (#33): the owner decided they ship from the v5.0.0
# sources without a hardware gate, because the 330 EVB has a dead click slot and
# the 510 EVB has a pin stuck in its click connector. Their parameters are still
# cross-checked four ways here and their helpers are diffed against the v4.1.0
# drop, but the images themselves are not flashed; BUILD-INFO says so per board.
for _b in "${BOARDS[@]}"; do
  case "$_b" in
    apollo510b_evb|apollo510_evb|apollo330mP_evb) ;;
    *) die "board ${_b} is not validated for release packaging; supported: apollo510b_evb apollo510_evb apollo330mP_evb" ;;
  esac
done

# Warn about validation notes that name a board this run does not package. The
# note lands in the board's own BUILD-INFO section, written when that board is
# packaged, so a stray note would silently do nothing.
_i=0
while [ "$_i" -lt "${#VALIDATION_BOARDS[@]}" ]; do
  _vb="${VALIDATION_BOARDS[$_i]}"
  _match=0
  for _b in "${BOARDS[@]}"; do [ "$_b" = "$_vb" ] && _match=1; done
  [ "$_match" -eq 0 ] && warn "--validation-note names ${_vb}, which is not being packaged in this run; it will be ignored"
  _i=$((_i + 1))
done

validation_note_for() {
  # validation_note_for <board>
  local i=0
  while [ "$i" -lt "${#VALIDATION_BOARDS[@]}" ]; do
    if [ "${VALIDATION_BOARDS[$i]}" = "$1" ]; then
      printf '%s' "${VALIDATION_TEXTS[$i]}"
      return 0
    fi
    i=$((i + 1))
  done
  printf '%s' "$DEFAULT_VALIDATION_NOTE"
}

cd "$REPO_DIR"

for tool in uv zip cmp diff; do
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

PKG_ROOT="dist/${TAG}"
# Per-board metadata for the top-level files, kept outside the package so it is
# neither shipped nor checksummed. It survives between runs so a board packaged
# earlier still gets its BUILD-INFO section and its FLASH.md table row.
STATE_DIR="dist/.${TAG}-state"
mkdir -p "$PKG_ROOT" "$STATE_DIR"

BUILD_LOG="$(mktemp -t nsx-build-log)"
trap 'rm -f "$BUILD_LOG"' EXIT

# ---------------------------------------------------------------------------
# Environment level provenance, the same for every board in this run
# ---------------------------------------------------------------------------

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

# The nsx CLI has no --version flag (argparse exits 2), so record the version
# of the installed neuralspotx distribution instead. Cross-checked below
# against tooling.nsx.version in nsx.yml.
NSX_VERSION="$(uv run python -c 'import importlib.metadata as m; print("neuralspotx " + m.version("neuralspotx"))' 2>/dev/null | first_line || true)"
[ -n "$NSX_VERSION" ] || NSX_VERSION="TODO(verify): could not read the installed neuralspotx version"
NSX_PINNED="$(awk '/^tooling:/{t=1} t&&/version:/{print $2; exit}' nsx.yml)"

GIT_COMMIT="$(git -C "$REPO_DIR" rev-parse HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY=""
if ! git -C "$REPO_DIR" diff --quiet HEAD -- 2>/dev/null; then GIT_DIRTY=" (tree dirty)"; fi
GIT_DESCRIBE="$(git -C "$REPO_DIR" describe --tags --always --dirty 2>/dev/null || true)"
[ -n "$GIT_DESCRIBE" ] || GIT_DESCRIBE="TODO(verify): git describe --tags --always --dirty failed"

# Whether the firmware sources still match the release tag. A packaged image
# built from a tree that has moved on from the tag is not the tagged firmware,
# so say which paths moved.
if git -C "$REPO_DIR" rev-parse -q --verify "refs/tags/${REF_TAG}" >/dev/null 2>&1; then
  if git -C "$REPO_DIR" diff --quiet "$REF_TAG" HEAD -- "${REF_PATHS[@]}" 2>/dev/null; then
    SOURCES_VS_TAG="yes"
  else
    _changed="$(git -C "$REPO_DIR" diff --name-only "$REF_TAG" HEAD -- "${REF_PATHS[@]}" 2>/dev/null | tr '\n' ' ')"
    SOURCES_VS_TAG="no (differs in: ${_changed% })"
  fi
else
  SOURCES_VS_TAG="TODO(verify): tag ${REF_TAG} not found in this checkout"
fi

# ---------------------------------------------------------------------------
# Per-board build and package
# ---------------------------------------------------------------------------

# render <template> <destination>, substituting from the R_* values.
render() {
  sed -e "s|@VERSION@|${VERSION}|g" \
      -e "s|@BOARD@|${R_BOARD}|g" \
      -e "s|@BOARD_DIR@|${R_BOARD_DIR}|g" \
      -e "s|@APP_NAME@|${APP_NAME}|g" \
      -e "s|@JLINK_DEVICE@|${R_DEVICE}|g" \
      -e "s|@SWD_SPEED@|${R_SPEED}|g" \
      -e "s|@LOAD_ADDRESS@|${R_ADDR}|g" \
      "$1" > "$2"
  if grep -q '@[A-Z_]*@' "$2"; then
    die "unsubstituted placeholder left in $2"
  fi
}

package_board() {
  local BOARD="$1"
  board_facts "$BOARD"

  local BUILD_DIR="build/${BOARD}"
  local BOARD_PKG="${PKG_ROOT}/${BOARD_DIR}"

  note "Building ${APP_NAME} for ${BOARD} (${VERSION})"

  # Force the app sources to recompile so the packaged image matches the tree.
  # Must recurse: src/generated/*.c is missed by a plain src/* glob.
  find src -type f -exec touch {} + || die "could not touch sources under src/"

  # Discard any previous build tree. Reusing one lets a stale toolchain path or
  # build type decide what ships. A fresh configure also makes --frozen mean
  # something: it re-resolves and fails on nsx.yml / nsx.lock / modules drift.
  # Only this board's build tree is removed; dist/<tag>/ keeps the other boards.
  rm -rf "$BUILD_DIR"

  : > "$BUILD_LOG"
  set +e
  uv run nsx configure --app-dir "$REPO_DIR" --board "$BOARD" --frozen 2>&1 | tee "$BUILD_LOG"
  local configure_rc="${PIPESTATUS[0]}"
  set -e
  [ "$configure_rc" -eq 0 ] || die "nsx configure --frozen exited with status ${configure_rc}"

  set +e
  uv run nsx build --app-dir "$REPO_DIR" --board "$BOARD" 2>&1 | tee -a "$BUILD_LOG"
  local build_rc="${PIPESTATUS[0]}"
  set -e
  [ "$build_rc" -eq 0 ] || die "nsx build exited with status ${build_rc}"

  if grep -nE 'error:|FAILED:' "$BUILD_LOG" >/dev/null 2>&1; then
    printf 'error: build log contains error or FAILED lines:\n' >&2
    grep -nE 'error:|FAILED:' "$BUILD_LOG" >&2
    exit 1
  fi

  local BIN_PATH="${BUILD_DIR}/${APP_NAME}.bin"
  local MAP_PATH="${BUILD_DIR}/${APP_NAME}.map"
  [ -f "$BIN_PATH" ] || die "build did not produce ${BIN_PATH}"
  [ -f "$MAP_PATH" ] || die "build did not produce ${MAP_PATH}"

  # -------------------------------------------------------------------------
  # J-Link parameters, read from the tool rather than assumed
  # -------------------------------------------------------------------------

  local FACTS_FILE="modules/nsx-ambiq-sdk/cmake/socs/facts/${SOC}.cmake"
  [ -f "$FACTS_FILE" ] || die "SoC facts file not found: ${FACTS_FILE}"

  local FACTS_DEVICE FACTS_SPEED LOAD_ADDRESS
  FACTS_DEVICE="$(sed -n "s/.*NSX_SEGGER_DEVICE[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$FACTS_FILE" | first_line)"
  FACTS_SPEED="$(sed -n "s/.*NSX_SEGGER_IF_SPEED[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$FACTS_FILE" | first_line)"
  LOAD_ADDRESS="$(sed -n "s/.*NSX_SEGGER_PF_ADDR[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$FACTS_FILE" | first_line)"

  [ -n "$FACTS_DEVICE" ]  || die "TODO(verify): NSX_SEGGER_DEVICE not found in ${FACTS_FILE}"
  [ -n "$FACTS_SPEED" ]   || die "TODO(verify): NSX_SEGGER_IF_SPEED not found in ${FACTS_FILE}"
  [ -n "$LOAD_ADDRESS" ]  || die "TODO(verify): NSX_SEGGER_PF_ADDR not found in ${FACTS_FILE}"

  # Board level override of the SoC facts. apollo330mP_evb pins the device to
  # the shared Apollo330P_510L fallback that installed J-Link packages know,
  # which is what the v4.1.0 drop shipped; the SoC facts name a part string
  # older J-Link packages do not carry.
  local DEBUG_CMAKE="boards/${BOARD}/debug.cmake"
  local OVERRIDE_DEVICE="" OVERRIDE_SPEED=""
  if [ -f "$DEBUG_CMAKE" ]; then
    OVERRIDE_DEVICE="$(sed -n 's/^[[:space:]]*set(NSX_SEGGER_DEVICE[[:space:]]*"\([^"]*\)".*/\1/p' "$DEBUG_CMAKE" | first_line)"
    OVERRIDE_SPEED="$(sed -n 's/^[[:space:]]*set(NSX_SEGGER_IF_SPEED[[:space:]]*"\{0,1\}\([^")]*\)"\{0,1\}).*/\1/p' "$DEBUG_CMAKE" | first_line)"
  fi

  # Cross-check 0: what CMake actually resolved for the flash target. This is
  # the value `nsx flash` would use, including any board override, so it is the
  # authoritative source for the helpers this script renders.
  local NINJA_FILE="${BUILD_DIR}/build.ninja"
  [ -f "$NINJA_FILE" ] || die "TODO(verify): ${NINJA_FILE} not found; cannot confirm the resolved J-Link parameters"
  local FLASH_CMD RESOLVED_DEVICE RESOLVED_SPEED RESOLVED_IF
  FLASH_CMD="$(grep -E "^[[:space:]]*COMMAND = .*JLink.*-commandfile [^ ]*flash_cmds\.jlink" "$NINJA_FILE" | first_line)"
  [ -n "$FLASH_CMD" ] || die "TODO(verify): no J-Link flash COMMAND found in ${NINJA_FILE}"
  RESOLVED_DEVICE="$(printf '%s\n' "$FLASH_CMD" | sed -n 's/.*-device[[:space:]]\{1,\}\([^[:space:]]*\).*/\1/p')"
  RESOLVED_SPEED="$(printf '%s\n' "$FLASH_CMD" | sed -n 's/.*-speed[[:space:]]\{1,\}\([^[:space:]]*\).*/\1/p')"
  RESOLVED_IF="$(printf '%s\n' "$FLASH_CMD" | sed -n 's/.*-if[[:space:]]\{1,\}\([^[:space:]]*\).*/\1/p')"
  [ -n "$RESOLVED_DEVICE" ] || die "TODO(verify): could not parse -device from the flash COMMAND in ${NINJA_FILE}"
  [ -n "$RESOLVED_SPEED" ]  || die "TODO(verify): could not parse -speed from the flash COMMAND in ${NINJA_FILE}"
  [ -n "$RESOLVED_IF" ]     || die "TODO(verify): could not parse -if from the flash COMMAND in ${NINJA_FILE}"

  # Expected values: the board override where there is one, the SoC facts
  # otherwise. Either way the resolved value has to agree, or the helpers would
  # disagree with `nsx flash`.
  local EXPECT_DEVICE="$FACTS_DEVICE" EXPECT_SPEED="$FACTS_SPEED"
  local DEVICE_SRC="SoC facts" SPEED_SRC="SoC facts"
  if [ -n "$OVERRIDE_DEVICE" ]; then EXPECT_DEVICE="$OVERRIDE_DEVICE"; DEVICE_SRC="board override"; fi
  if [ -n "$OVERRIDE_SPEED" ];  then EXPECT_SPEED="$OVERRIDE_SPEED";   SPEED_SRC="board override"; fi

  if [ "$RESOLVED_DEVICE" != "$EXPECT_DEVICE" ] || [ "$RESOLVED_SPEED" != "$EXPECT_SPEED" ] || [ "$RESOLVED_IF" != "$JLINK_IF" ]; then
    cat >&2 <<PARAMS
error: the J-Link parameters CMake resolved differ from the expected sources.
Stopping rather than shipping helpers that disagree with \`nsx flash\`.
  expected (device from ${DEVICE_SRC}, speed from ${SPEED_SRC}) : device=${EXPECT_DEVICE} if=${JLINK_IF} speed=${EXPECT_SPEED}
  ${FACTS_FILE}            : device=${FACTS_DEVICE} speed=${FACTS_SPEED}
  ${DEBUG_CMAKE}           : device=${OVERRIDE_DEVICE:-(none)} speed=${OVERRIDE_SPEED:-(none)}
  ${NINJA_FILE} (resolved) : device=${RESOLVED_DEVICE} if=${RESOLVED_IF} speed=${RESOLVED_SPEED}
PARAMS
    exit 1
  fi

  # The helpers render from the resolved values.
  local JLINK_DEVICE="$RESOLVED_DEVICE" SWD_SPEED="$RESOLVED_SPEED"

  # Cross-check 1: the linker script the build actually uses.
  local LD_PATH="modules/nsx-ambiq-sdk/modules/nsx-core/src/${SOC}/gcc/linker_script_sbl.ld"
  local LD_ORIGIN=""
  if [ -f "$LD_PATH" ]; then
    LD_ORIGIN="$(sed -n 's/.*MCU_MRAM[^:]*:[[:space:]]*ORIGIN[[:space:]]*=[[:space:]]*\(0[xX][0-9A-Fa-f]*\).*/\1/p' "$LD_PATH" | first_line)"
  fi
  [ -n "$LD_ORIGIN" ] || die "TODO(verify): could not read MCU_MRAM ORIGIN from ${LD_PATH}"

  # Cross-check 2: the command file NSX generated for this very build.
  local GEN_JLINK="${BUILD_DIR}/jlink/${APP_NAME}/flash_cmds.jlink"
  local GEN_ADDR=""
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

  note "J-Link (${BOARD}): device=${JLINK_DEVICE} if=${JLINK_IF} speed=${SWD_SPEED} kHz addr=${LOAD_ADDRESS}"

  # -------------------------------------------------------------------------
  # Assemble this board's folder
  # -------------------------------------------------------------------------

  # Only this board's folder is cleared, so a board packaged in an earlier run
  # keeps its files.
  rm -rf "$BOARD_PKG"
  mkdir -p "$BOARD_PKG"

  cp "$BIN_PATH" "${BOARD_PKG}/firmware.bin"
  chmod 644 "${BOARD_PKG}/firmware.bin"
  cp "$MAP_PATH" "${PKG_ROOT}/${BOARD_DIR}-firmware.map"
  chmod 644 "${PKG_ROOT}/${BOARD_DIR}-firmware.map"

  R_BOARD="$BOARD"; R_BOARD_DIR="$BOARD_DIR"
  R_DEVICE="$JLINK_DEVICE"; R_SPEED="$SWD_SPEED"; R_ADDR="$LOAD_ADDRESS"
  render "${TEMPLATE_DIR}/downloadfw.jlink"   "${BOARD_PKG}/downloadfw.jlink"
  render "${TEMPLATE_DIR}/flash_mac.command"  "${BOARD_PKG}/flash_mac.command"
  render "${TEMPLATE_DIR}/flash_linux.sh"     "${BOARD_PKG}/flash_linux.sh"
  render "${TEMPLATE_DIR}/flash_win.bat"      "${BOARD_PKG}/flash_win.bat"

  # flash_win.bat keeps LF line endings, byte-for-byte matching the v410 drop the
  # FAEs already use. Do not "fix" this to CRLF without re-validating on Windows.

  chmod 755 "${BOARD_PKG}/flash_mac.command" "${BOARD_PKG}/flash_linux.sh"
  chmod 644 "${BOARD_PKG}/flash_win.bat" "${BOARD_PKG}/downloadfw.jlink"

  local V410_RESULT
  V410_RESULT="$(compare_against_v410 "$BOARD_DIR" "$BOARD_PKG")"

  # Dependency provenance. The app repo's dirty flag does not cover modules/,
  # which is ignored via modules/.gitignore, so check it separately. This exits
  # non-zero and stops the package if a module diverges from nsx.lock.
  local MODULES_REPORT
  MODULES_REPORT="$(uv run python "${SCRIPT_DIR}/verify_modules.py" "$REPO_DIR" "$BOARD")" \
    || die "dependency module check failed; see the message above"

  # Toolchain and build type actually used, straight from the configured cache.
  local CACHE_FILE="${BUILD_DIR}/CMakeCache.txt"
  local CMAKE_TOOLCHAIN CMAKE_BUILD_TYPE
  CMAKE_TOOLCHAIN="$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[^=]*=\(.*\)$/\1/p' "$CACHE_FILE" 2>/dev/null | first_line)"
  CMAKE_BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=\(.*\)$/\1/p' "$CACHE_FILE" 2>/dev/null | first_line)"
  [ -n "$CMAKE_TOOLCHAIN" ]  || CMAKE_TOOLCHAIN="TODO(verify): not found in ${CACHE_FILE}"
  [ -n "$CMAKE_BUILD_TYPE" ] || CMAKE_BUILD_TYPE="(unset)"

  local BIN_SHA BIN_SIZE
  BIN_SHA="$(sha256_of "${BOARD_PKG}/firmware.bin")"
  BIN_SIZE="$(wc -c < "${BOARD_PKG}/firmware.bin" | tr -d ' ')"

  local VALIDATION
  VALIDATION="$(validation_note_for "$BOARD")"

  # Device provenance. With a board override the resolved value ships and the
  # SoC facts default is recorded next to it, so a reader can see both.
  local DEVICE_LINE DEVICE_SOURCES
  if [ -n "$OVERRIDE_DEVICE" ]; then
    DEVICE_LINE="${JLINK_DEVICE} (SoC facts default: ${FACTS_DEVICE}, board override from ${DEBUG_CMAKE})"
    DEVICE_SOURCES="                 ${DEBUG_CMAKE} (NSX_SEGGER_DEVICE), confirmed against
                 the resolved flash command in ${NINJA_FILE}"
  else
    DEVICE_LINE="${JLINK_DEVICE}"
    DEVICE_SOURCES="                 ${FACTS_FILE} (NSX_SEGGER_DEVICE)
                 confirmed against the resolved flash command in ${NINJA_FILE}"
  fi
  local SPEED_SOURCES
  if [ -n "$OVERRIDE_SPEED" ]; then
    SPEED_SOURCES="                 ${DEBUG_CMAKE} (NSX_SEGGER_IF_SPEED), SoC facts default ${FACTS_SPEED},
                 confirmed against the resolved flash command in ${NINJA_FILE}"
  else
    SPEED_SOURCES="                 ${FACTS_FILE} (NSX_SEGGER_IF_SPEED)
                 confirmed against the resolved flash command in ${NINJA_FILE}"
  fi

  # BUILD-INFO section for this board, kept so a later run for another board can
  # rebuild the whole file without rebuilding this board.
  cat > "${STATE_DIR}/${BOARD_DIR}.section" <<SECTION
------------------------------------------------------------------------
Board ${BOARD} (folder ${BOARD_DIR})
------------------------------------------------------------------------

SoC            : ${SOC}
Built (UTC)    : $(date -u '+%Y-%m-%d %H:%M:%SZ')
Hardware validation: ${VALIDATION}

Provenance
----------
Git commit     : ${GIT_COMMIT}${GIT_DIRTY}
Git describe   : ${GIT_DESCRIBE}
Firmware sources identical to tag ${REF_TAG}: ${SOURCES_VS_TAG}
                 (compared paths: ${REF_PATHS[*]})
NSX toolchain  : ${NSX_VERSION}
nsx.yml pin    : tooling.nsx.version = ${NSX_PINNED}
Build command  : uv run nsx configure --app-dir . --board ${BOARD} --frozen
                 uv run nsx build --app-dir . --board ${BOARD}
                 (built from a removed and freshly configured build tree)
Toolchain file : ${CMAKE_TOOLCHAIN}
Build type     : ${CMAKE_BUILD_TYPE}

${MODULES_REPORT}

Image
-----
firmware.bin   : ${BIN_SIZE} bytes
SHA-256        : ${BIN_SHA}

J-Link parameters and their sources
-----------------------------------
Device         : ${DEVICE_LINE}
${DEVICE_SOURCES}
Interface      : ${JLINK_IF}
                 ${IF_SOURCE}
Speed          : ${SWD_SPEED} kHz
${SPEED_SOURCES}
Load address   : ${LOAD_ADDRESS}
                 ${FACTS_FILE} (NSX_SEGGER_PF_ADDR)
                 cross-checked against ${LD_PATH} (MCU_MRAM ORIGIN = ${LD_ORIGIN})
                 cross-checked against ${GEN_JLINK} (LoadFile ${GEN_ADDR})

Command sequence in downloadfw.jlink matches the NSX-generated flash command
file for this build: ExitOnError, Reset, LoadFile, Reset, Go, Exit.
Helpers vs the v4.1.0 drop: ${V410_RESULT}
SECTION

  # Values the top-level FLASH.md table needs.
  cat > "${STATE_DIR}/${BOARD_DIR}.env" <<ENV
BOARD=${BOARD}
BOARD_DIR=${BOARD_DIR}
DEVICE=${JLINK_DEVICE}
SPEED=${SWD_SPEED}
ADDR=${LOAD_ADDRESS}
ENV
}

# compare_against_v410 <board-dir> <package-dir>
# Byte comparison of the rendered helpers against the field-proven v4.1.0 drop.
# A difference is a loud warning, not a failure: the templates carry a version
# banner that legitimately moves between releases. Prints the one line summary
# that goes into BUILD-INFO on stdout, everything else on stderr.
compare_against_v410() {
  local board_dir="$1" pkg_dir="$2"
  local ref="${V410_REF_DIR}/${board_dir}"
  local f matched=0 differed=0 missing=0 diff_list=""

  if [ ! -d "$ref" ]; then
    printf 'not compared (v4.1.0 reference not present at %s)' "$ref"
    warn "v4.1.0 reference folder not found, skipping the byte comparison for ${board_dir}: ${ref}"
    return 0
  fi

  printf '==> v4.1.0 byte comparison for %s against %s\n' "$board_dir" "$ref" >&2
  for f in downloadfw.jlink flash_mac.command flash_win.bat flash_linux.sh; do
    if [ ! -f "${ref}/${f}" ]; then
      printf '    %-18s MISSING in the v4.1.0 reference\n' "$f" >&2
      missing=$((missing + 1))
      diff_list="${diff_list}${f}(missing) "
      continue
    fi
    if cmp -s "${pkg_dir}/${f}" "${ref}/${f}"; then
      printf '    %-18s MATCH\n' "$f" >&2
      matched=$((matched + 1))
    else
      printf '    %-18s DIFF\n' "$f" >&2
      differed=$((differed + 1))
      diff_list="${diff_list}${f} "
      diff -u "${ref}/${f}" "${pkg_dir}/${f}" >&2 || true
    fi
  done

  if [ "$differed" -gt 0 ] || [ "$missing" -gt 0 ]; then
    printf '%s\n' \
      '**********************************************************************' \
      "WARNING: ${board_dir} helpers differ from the v4.1.0 drop: ${diff_list% }" \
      'Review the diff above. A version banner change is expected; anything' \
      'touching the J-Link device, speed, or load address is not.' \
      '**********************************************************************' >&2
    printf '%d match, %d differ (%s), %d missing in the reference' \
      "$matched" "$differed" "${diff_list% }" "$missing"
  else
    printf 'all %d files byte identical' "$matched"
  fi
}

for _board in "${BOARDS[@]}"; do
  package_board "$_board"
done

# ---------------------------------------------------------------------------
# Top-level files, written once for the whole drop
# ---------------------------------------------------------------------------

# Every board folder present, including any from an earlier run.
BOARD_DIRS=()
while IFS= read -r _d; do
  [ -n "$_d" ] && BOARD_DIRS+=("$(basename "$_d")")
done < <(find "$PKG_ROOT" -mindepth 1 -maxdepth 1 -type d | LC_ALL=C sort)
[ "${#BOARD_DIRS[@]}" -gt 0 ] || die "no board folders in ${PKG_ROOT}"

for _d in "${BOARD_DIRS[@]}"; do
  [ -f "${STATE_DIR}/${_d}.env" ] \
    || die "no packaging metadata for ${PKG_ROOT}/${_d}; re-run package.sh for that board (state lives in ${STATE_DIR})"
done

state_value() {
  # state_value <board-dir> <key>
  sed -n "s/^$2=//p" "${STATE_DIR}/$1.env" | first_line
}

# FLASH.md. With a single board it renders exactly as it always has. With more
# than one, the board specific values become placeholders and a table of the
# boards in the drop is appended, because one set of instructions has to serve
# every folder.
if [ "${#BOARD_DIRS[@]}" -eq 1 ]; then
  R_BOARD="$(state_value "${BOARD_DIRS[0]}" BOARD)"
  R_BOARD_DIR="${BOARD_DIRS[0]}"
  R_DEVICE="$(state_value "${BOARD_DIRS[0]}" DEVICE)"
  R_SPEED="$(state_value "${BOARD_DIRS[0]}" SPEED)"
  R_ADDR="$(state_value "${BOARD_DIRS[0]}" ADDR)"
  render "${TEMPLATE_DIR}/FLASH.md" "${PKG_ROOT}/FLASH.md"
else
  _names=""
  _addr_common="$(state_value "${BOARD_DIRS[0]}" ADDR)"
  for _d in "${BOARD_DIRS[@]}"; do
    _names="${_names}, $(state_value "$_d" BOARD)"
    [ "$(state_value "$_d" ADDR)" = "$_addr_common" ] || _addr_common="<load address>"
  done
  R_BOARD="${_names#, }"
  R_BOARD_DIR="<board>"
  R_DEVICE="<device>"
  R_SPEED="<speed>"
  R_ADDR="$_addr_common"
  render "${TEMPLATE_DIR}/FLASH.md" "${PKG_ROOT}/FLASH.md"

  {
    printf '\n## Boards in this package\n\n'
    printf 'Each board has its own folder. Substitute the row for the board being\n'
    printf 'flashed wherever the instructions above show `<board>`, `<device>`,\n'
    printf '`<speed>` or `<load address>`.\n\n'
    printf '| Folder | NSX board | J-Link device | Speed (kHz) | Load address |\n'
    printf '| --- | --- | --- | --- | --- |\n'
    for _d in "${BOARD_DIRS[@]}"; do
      printf '| `%s` | `%s` | `%s` | %s | `%s` |\n' \
        "$_d" "$(state_value "$_d" BOARD)" "$(state_value "$_d" DEVICE)" \
        "$(state_value "$_d" SPEED)" "$(state_value "$_d" ADDR)"
    done
  } >> "${PKG_ROOT}/FLASH.md"
fi
chmod 644 "${PKG_ROOT}/FLASH.md"

# RELEASE.md. Real release notes replace the placeholder before SHA256SUMS is
# written, so the checksum always covers the text that actually ships.
R_BOARD="${BOARD_DIRS[0]}"; R_BOARD_DIR="${BOARD_DIRS[0]}"
R_DEVICE="$(state_value "${BOARD_DIRS[0]}" DEVICE)"
R_SPEED="$(state_value "${BOARD_DIRS[0]}" SPEED)"
R_ADDR="$(state_value "${BOARD_DIRS[0]}" ADDR)"
render "${TEMPLATE_DIR}/RELEASE.md" "${PKG_ROOT}/RELEASE.md"
if [ -n "$NOTES_FILE" ]; then
  cp "$NOTES_FILE" "${PKG_ROOT}/RELEASE.md"
  note "Release notes taken from ${NOTES_FILE}"
fi
chmod 644 "${PKG_ROOT}/RELEASE.md"

# BUILD-INFO.txt: one drop-level header plus the section each board wrote when
# it was packaged.
{
  cat <<HEADER
HeartKit Vitals demo firmware package
=====================================

Version        : ${VERSION}
Package tag    : ${TAG}
Application    : ${APP_NAME}
Boards         : ${BOARD_DIRS[*]}
Assembled (UTC): $(date -u '+%Y-%m-%d %H:%M:%SZ')

Each board is built and packaged by its own run of tools/release/package.sh,
so the provenance below is recorded per board and reflects the tree that board
was built from.

HEADER
  for _d in "${BOARD_DIRS[@]}"; do
    cat "${STATE_DIR}/${_d}.section"
    printf '\n'
  done
} > "${PKG_ROOT}/BUILD-INFO.txt"
chmod 644 "${PKG_ROOT}/BUILD-INFO.txt"

# SHA256SUMS covers every packaged file, with paths relative to the package
# root so `shasum -a 256 -c SHA256SUMS` works from the extracted folder. Written
# last, and regenerated over every board present, not just the ones built now.
( cd "$PKG_ROOT" && find . -type f ! -name SHA256SUMS -print0 \
    | LC_ALL=C sort -z \
    | xargs -0 "${SHA_CMD[@]}" > SHA256SUMS )

# ---------------------------------------------------------------------------
# Zip and report
# ---------------------------------------------------------------------------

ZIPS=()

# One zip per board: the top-level files plus that board's folder and map. Its
# SHA256SUMS is narrowed to what the archive actually contains, otherwise
# `shasum -c` inside a single-board extraction reports the other boards' files
# as failures.
for _d in "${BOARD_DIRS[@]}"; do
  _stage="dist/.${TAG}-zip-${_d}"
  rm -rf "$_stage"
  mkdir -p "${_stage}/${TAG}"
  cp -p "${PKG_ROOT}/RELEASE.md" "${PKG_ROOT}/FLASH.md" "${PKG_ROOT}/BUILD-INFO.txt" "${_stage}/${TAG}/"
  cp -p "${PKG_ROOT}/${_d}-firmware.map" "${_stage}/${TAG}/"
  cp -Rp "${PKG_ROOT}/${_d}" "${_stage}/${TAG}/"
  ( cd "${_stage}/${TAG}" && find . -type f ! -name SHA256SUMS -print0 \
      | LC_ALL=C sort -z \
      | xargs -0 "${SHA_CMD[@]}" > SHA256SUMS )
  _zip="${APP_NAME}-${TAG}-${_d}-firmware.zip"
  ( cd "$_stage" && rm -f "../${_zip}" && zip -q -r -X "../${_zip}" "$TAG" )
  rm -rf "$_stage"
  ZIPS+=("dist/${_zip}")
done

# Combined zip covering the whole drop, only when there is more than one board.
# This name is the one already attached to the v5.0.0 GitHub release.
if [ "${#BOARD_DIRS[@]}" -gt 1 ]; then
  COMBINED_ZIP="${APP_NAME}-${TAG}-firmware.zip"
  ( cd dist && rm -f "$COMBINED_ZIP" && zip -q -r -X "$COMBINED_ZIP" "$TAG" )
  ZIPS+=("dist/${COMBINED_ZIP}")
fi

note "Package ready"
printf '\n'
printf 'Package  : %s\n' "$PKG_ROOT"
printf 'Boards   : %s\n' "${BOARD_DIRS[*]}"
for _z in "${ZIPS[@]}"; do
  printf 'Archive  : %s\n' "$_z"
  printf 'SHA-256  : %s\n' "$(sha256_of "$_z")"
done
printf '\n'

if [ -z "$NOTES_FILE" ]; then
  printf '%s\n' \
    '**********************************************************************' \
    'WARNING: this package ships the RELEASE.md placeholder, not release' \
    'notes. It is not ready to hand to an FAE. Re-run with --notes FILE.' \
    '**********************************************************************' >&2
fi
