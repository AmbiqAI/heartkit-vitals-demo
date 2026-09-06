#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
#
# Publish a built firmware drop: release assets, release notes, and the shared
# drop folder an FAE downloads from.
#
#   tools/release/publish.sh --tag v5.0.0 \
#                            --notes RELEASE-NOTES-v5.0.0.md \
#                            --dest "$HOME/.../firmware/v500" \
#                            [--dry-run | --yes] [--allow-published]
#
# Run ONLY after the drop has been built from the release commit by
# tools/release/package.sh and the packaged binary has been flashed through its
# own helper and read on the board. The hardware gate is recorded in the
# release notes first; see the release gate section of docs/developer.md.
#
# Nothing is mutated without --yes. Without it, and with --dry-run, the script
# runs every read-only check and prints the plan it would carry out.
#
# The GitHub release must still be a draft. Publishing over a release people
# may already have downloaded needs --allow-published and a typed confirmation
# at the terminal, which --yes does not stand in for.
#
# The drop folder is replaced through a staged swap rather than in place: the
# new contents are assembled in a sibling directory, the live folder is renamed
# to a backup, and the staging directory takes its place. Any failure restores
# the backup, so a shared folder is never left half replaced; a restore that
# cannot be carried out is reported as COULD NOT RESTORE, naming both paths for
# the operator to sort out by hand. FAE-RUNBOOK.md is
# written by the field team, not by this repo, so the copy in the destination is
# carried across.
#
# The drop is swapped and the destination verified before the release is
# touched, because the swap is the step that can be undone and the release is
# not: an upload people may already have fetched cannot be recalled. If the gh
# step then fails the new drop stays in place, correct and verified, and the
# run stops with the gh commands to retry by hand. See #70.
#
# See #62.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
plan() { printf '    %s\n' "$*"; }

usage() {
  cat <<'USAGE'
Usage: tools/release/publish.sh --tag vX.Y.Z --notes FILE --dest DIR
                                [--repo-dir DIR] [--dry-run] [--yes]
                                [--allow-published]

  --tag              Release tag, for example v5.0.0. The dist slug is the tag
                     with dots removed (v5.0.0 -> v500), matching package.sh.
                     Required.
  --notes            Release notes markdown, passed to `gh release edit -F`.
                     Required: the notes carry the hardware gate result.
  --dest             Destination drop folder. Must be an existing directory at
                     least two levels below $HOME. Its contents are replaced,
                     except FAE-RUNBOOK.md, which is preserved. Required.
  --repo-dir         Repository holding dist/. Default: the repository this
                     script lives in.
  --dry-run          Print the plan and exit without mutating anything, even
                     with --yes.
  --yes              Carry the plan out. Without it the plan is printed and
                     nothing is uploaded, copied or deleted.
  --allow-published  Allow a release that is no longer a draft. Off by default:
                     republishing over a release people may already have
                     downloaded is a deliberate act. Prompts at the terminal
                     for a typed `yes` before anything is mutated; --yes does
                     not answer that prompt, and a run with no terminal to ask
                     at stops instead.

The destination is swapped and verified first; the release is uploaded and its
notes set only after that. A gh failure after the swap leaves the new drop in
place and prints the commands to retry by hand.

Exit codes:
  0  Plan printed, or destination refreshed, assets uploaded and notes set.
  1  A precondition failed, a checksum did not verify, the swap was rolled
     back, the published-release confirmation was declined, or the release
     could not be updated after the destination was swapped.
  2  Bad arguments.
USAGE
}

TAG=""
NOTES=""
DEST_ARG=""
REPO_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
DRY_RUN=0
CONFIRMED=0
ALLOW_PUBLISHED=0

while [ $# -gt 0 ]; do
  case "$1" in
    --tag)      [ $# -ge 2 ] || die "--tag needs a value";      TAG="$2"; shift 2 ;;
    --notes)    [ $# -ge 2 ] || die "--notes needs a value";    NOTES="$2"; shift 2 ;;
    --dest)     [ $# -ge 2 ] || die "--dest needs a value";     DEST_ARG="$2"; shift 2 ;;
    --repo-dir) [ $# -ge 2 ] || die "--repo-dir needs a value"; REPO_DIR="$2"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    --yes) CONFIRMED=1; shift ;;
    --allow-published) ALLOW_PUBLISHED=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; printf 'error: unknown argument: %s\n' "$1" >&2; exit 2 ;;
  esac
done

[ -n "$TAG" ]      || { usage >&2; printf 'error: --tag is required\n' >&2; exit 2; }
[ -n "$NOTES" ]    || { usage >&2; printf 'error: --notes is required\n' >&2; exit 2; }
[ -n "$DEST_ARG" ] || { usage >&2; printf 'error: --dest is required\n' >&2; exit 2; }

[[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+([-.+][0-9A-Za-z.-]+)?$ ]] \
  || die "tag must look like v5.0.0 (optional -suffix), got: $TAG"

# Slug: v5.0.0 -> v500, v0.0.0-test -> v000-test. Matches package.sh.
_core="${TAG#v}"
_base="${_core%%-*}"
_suffix="${_core#"$_base"}"
SLUG="v${_base//./}${_suffix}"

[ -d "$REPO_DIR" ] || die "--repo-dir is not a directory: $REPO_DIR"
REPO_DIR="$(cd -- "$REPO_DIR" && pwd)"
cd "$REPO_DIR"

command -v gh >/dev/null 2>&1 || die "required tool not found on PATH: gh"
command -v shasum >/dev/null 2>&1 || die "required tool not found on PATH: shasum"

# ---------------------------------------------------------------------------
# Destination guard
# ---------------------------------------------------------------------------

# The destination is a shared folder whose contents this script removes, so it
# is checked before anything else is read. A trailing slash and a path with a
# missing component are rejected rather than silently canonicalised: both are
# the shape a mistyped path takes, and either would put the staging and backup
# directories somewhere other than beside the folder the operator meant.
guard_destination() {
  local raw="$1"
  case "$raw" in
    */) die "--dest must not end in a slash: $raw" ;;
  esac
  [ -e "$raw" ] || die "--dest is not an existing directory (missing component?): $raw"
  [ -d "$raw" ] || die "--dest is not a directory: $raw"

  local real home_real rel depth
  real="$(cd -P -- "$raw" 2>/dev/null && pwd)" \
    || die "--dest could not be resolved: $raw"
  [ -n "${HOME:-}" ] || die "HOME is not set; cannot check the destination"
  home_real="$(cd -P -- "$HOME" 2>/dev/null && pwd)" \
    || die "HOME does not resolve to a directory: $HOME"

  case "$real" in
    "$home_real") die "refusing to replace \$HOME itself: $real" ;;
    "$home_real"/*) rel="${real#"$home_real"/}" ;;
    *) die "--dest is outside \$HOME: $real" ;;
  esac

  # A drop folder lives under a share root, so it is never a direct child of
  # $HOME. One level below is far more likely to be a mistyped path than a real
  # destination, and the whole folder gets removed.
  depth="$(printf '%s' "$rel" | awk -F/ '{print NF}')"
  [ "$depth" -ge 2 ] \
    || die "--dest must be at least two levels below \$HOME, got: ~/${rel}"

  # The drop folder is named for the release it holds, so a destination whose
  # name is not the slug is pointing at the wrong release.
  [ "$(basename "$real")" = "$SLUG" ] \
    || die "--dest must be named ${SLUG} for tag ${TAG}, got: $(basename "$real")"

  printf '%s' "$real"
}

DEST="$(guard_destination "$DEST_ARG")"
DEST_PARENT="$(dirname "$DEST")"
DEST_NAME="$(basename "$DEST")"
[ -w "$DEST_PARENT" ] || die "cannot write beside the destination: $DEST_PARENT"

STAGING="${DEST_PARENT}/.${DEST_NAME}.staging"
BACKUP="${DEST_PARENT}/.${DEST_NAME}.backup"

# A leftover backup is either the only surviving copy of a destination an
# earlier run failed to restore, or the remains of one a successful run was
# interrupted while deleting. Checked here rather than at the swap, so the stop
# lands before anything is uploaded. See #70.
[ ! -e "$BACKUP" ] || die "a backup from an earlier run is in the way: ${BACKUP}
it holds either the previous destination (an earlier run could not restore it)
or a partly deleted backup left by a run that published successfully; compare it
against ${DEST} and remove it by hand before publishing again"

STAGING_LIVE=0
# Set once the destination holds the new drop and has verified. From that point
# the folder is correct, so nothing that follows may roll it back. See #70.
SWAP_VERIFIED=0
CLEANUP_DONE=0

# ---------------------------------------------------------------------------
# Source drop
# ---------------------------------------------------------------------------

PKG_ROOT="dist/${SLUG}"
[ -f "$NOTES" ] || die "release notes not found: $NOTES"
[ -s "$NOTES" ] || die "release notes are empty: $NOTES"
[ -d "$PKG_ROOT" ] || die "no drop folder ${PKG_ROOT}; run tools/release/package.sh first"
[ -f "${PKG_ROOT}/SHA256SUMS" ] || die "${PKG_ROOT}/SHA256SUMS is missing; the drop cannot be verified"

# The archive names package.sh writes for this slug, spelled out: the combined
# drop and one per board folder in the drop. A glob on the slug would also
# match the archives of a suffixed release, uploading v500-rc1 as v500. See #70.
_CANDIDATES=("heartkit-vitals-demo-${SLUG}-firmware.zip")
while IFS= read -r _board; do
  [ -n "$_board" ] || continue
  _CANDIDATES+=("heartkit-vitals-demo-${SLUG}-${_board}-firmware.zip")
done < <(find "$PKG_ROOT" -mindepth 1 -maxdepth 1 -type d | sed 's|.*/||')

ZIPS=()
while IFS= read -r _zip; do
  if [ -f "dist/${_zip}" ]; then ZIPS+=("dist/${_zip}"); fi
done < <(printf '%s\n' "${_CANDIDATES[@]}" | LC_ALL=C sort -u)
[ "${#ZIPS[@]}" -gt 0 ] \
  || die "no dist/heartkit-vitals-demo-${SLUG}-firmware.zip or per-board archive to upload"

# ---------------------------------------------------------------------------
# Cleanup and rollback
# ---------------------------------------------------------------------------

on_exit() {
  [ "$CLEANUP_DONE" -eq 0 ] || return 0
  CLEANUP_DONE=1

  # The exit status is deliberately not consulted: bash reports rc=0 to the
  # EXIT trap when a signal ends the run, and a half-finished swap has to be
  # undone either way. See #70.
  # Keyed on the backup directory rather than a flag, so an interrupt landing
  # inside the rename that creates it still restores. The pre-flight check
  # guarantees any backup present here was made by this run. See #70.
  if [ "$SWAP_VERIFIED" -eq 0 ] && [ -d "$BACKUP" ]; then
    warn "publish failed mid-swap; restoring the previous destination from ${BACKUP}"
    rm -rf "${DEST:?}" || true
    if [ ! -e "$DEST" ] && mv "$BACKUP" "$DEST"; then
      warn "destination restored from ${BACKUP} (including FAE-RUNBOOK.md)"
    else
      warn "COULD NOT RESTORE: the previous destination is at ${BACKUP} and ${DEST} is in the way; move ${DEST} aside and rename ${BACKUP} back by hand"
    fi
  fi
  if [ "$STAGING_LIVE" -eq 1 ] && [ -d "$STAGING" ]; then
    rm -rf "$STAGING" 2>/dev/null || true
  fi
  if [ -n "${TMP_DIR:-}" ] && [ -d "$TMP_DIR" ]; then
    rm -rf "$TMP_DIR" 2>/dev/null || true
  fi
}
# Installed before the first temporary directory exists, so a pre-flight die
# does not leak one.
trap on_exit EXIT
# Without an explicit exit the signal handler would return into the interrupted
# swap and carry on.
trap 'on_exit; exit 130' INT
trap 'on_exit; exit 143' TERM

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/hkv-publish.XXXXXX")"

# One checklist for everything that lands in the destination: the drop tree from
# the package's own SHA256SUMS, plus the archives, which are built beside the
# tree and so are not covered by it. Paths stay relative, so the same file
# verifies the staging directory and the destination.
ALL_SUMS="${TMP_DIR}/PUBLISHED-SUMS"
cp "${PKG_ROOT}/SHA256SUMS" "$ALL_SUMS"
( cd dist && shasum -a 256 "${ZIPS[@]#dist/}" ) >> "$ALL_SUMS"

# verify_sums <dir> <checklist> <what>
verify_sums() {
  local dir="$1" list="$2" what="$3" out
  if ! out="$(cd "$dir" && shasum -a 256 -c "$list" 2>&1)"; then
    printf '%s\n' "$out" | grep -v ': OK$' >&2 || true
    die "checksums do not verify in ${what}"
  fi
  note "checksums OK (${what})"
}

verify_sums "$PKG_ROOT" SHA256SUMS "$PKG_ROOT"

# A shipped text file naming a build machine leaks the packager's paths. Linker
# maps are excluded: package.sh scrubs and re-checks those itself, and they are
# a debugging aid rather than something an FAE reads. See #40.
#
# grep exits 1 for "no match" and 2 for "could not read something", so the two
# are told apart. A scan that failed to run has proved nothing.
set +e
SCAN_OUT="$(grep -rlE --exclude=firmware.bin --exclude='*.map' '(/Users/|/home/)' "$PKG_ROOT" 2>&1)"
SCAN_RC=$?
set -e
case "$SCAN_RC" in
  0) printf '%s\n' "$SCAN_OUT" >&2; die "local path in shipped text" ;;
  1) note "no local paths in shipped text (binaries carry __FILE__ paths)" ;;
  *) printf '%s\n' "$SCAN_OUT" >&2; die "local path scan failed (grep exited ${SCAN_RC})" ;;
esac

# ---------------------------------------------------------------------------
# Release gate
# ---------------------------------------------------------------------------

IS_DRAFT="$(gh release view "$TAG" --json isDraft --jq '.isDraft' 2>/dev/null || true)"
case "$IS_DRAFT" in
  true) RELEASE_STATE="draft" ;;
  false)
    RELEASE_STATE="published"
    [ "$ALLOW_PUBLISHED" -eq 1 ] \
      || die "release ${TAG} is already published; re-run with --allow-published to replace its assets"
    warn "release ${TAG} is already published; --allow-published was given"
    ;;
  *) die "could not read the draft state of release ${TAG} from gh" ;;
esac

# ---------------------------------------------------------------------------
# Plan
# ---------------------------------------------------------------------------

NEW_LIST="${TMP_DIR}/new"
OLD_LIST="${TMP_DIR}/old"
DELETE_LIST="${TMP_DIR}/delete"

( cd "$PKG_ROOT" && find . -type f | sed 's|^\./||' ) | LC_ALL=C sort > "$NEW_LIST"
printf '%s\n' "${ZIPS[@]#dist/}" >> "$NEW_LIST"
# FAE-RUNBOOK.md is carried across, so it is never a deletion.
printf 'FAE-RUNBOOK.md\n' >> "$NEW_LIST"
LC_ALL=C sort -o "$NEW_LIST" "$NEW_LIST"

( cd "$DEST" && find . -type f | sed 's|^\./||' ) | LC_ALL=C sort > "$OLD_LIST"
LC_ALL=C comm -23 "$OLD_LIST" "$NEW_LIST" > "$DELETE_LIST"

KEEP_RUNBOOK=0
[ -f "${DEST}/FAE-RUNBOOK.md" ] && KEEP_RUNBOOK=1

printf '\n'
note "publish plan for ${TAG}"
printf '\n'
printf '  release\n'
plan "tag           : ${TAG} (${RELEASE_STATE})"
plan "notes         : ${NOTES}"
plan "source drop   : ${REPO_DIR}/${PKG_ROOT}"
printf '  step 1: destination\n'
plan "folder        : ${DEST}"
plan "staging       : ${STAGING}"
plan "backup        : ${BACKUP}"
printf '  files to copy (%s)\n' "$(grep -cve '^FAE-RUNBOOK\.md$' "$NEW_LIST" || true)"
while IFS= read -r _f; do
  [ "$_f" = "FAE-RUNBOOK.md" ] && continue
  plan "$_f"
done < "$NEW_LIST"
if [ "$KEEP_RUNBOOK" -eq 1 ]; then
  printf '  files preserved from the destination\n'
  plan "FAE-RUNBOOK.md"
fi
printf '  files to delete (%s)\n' "$(wc -l < "$DELETE_LIST" | tr -d ' ')"
while IFS= read -r _f; do plan "$_f"; done < "$DELETE_LIST"
printf '  checksums to verify\n'
plan "${PKG_ROOT}/SHA256SUMS plus ${#ZIPS[@]} archive digests, checked in the"
plan "staging directory and again in the destination after the swap"
printf '  step 2: assets to upload (gh release upload --clobber)\n'
for _z in "${ZIPS[@]}"; do plan "$_z"; done
plan "then gh release edit ${TAG} -F ${NOTES}"
printf '  order\n'
plan "the destination is swapped and verified before the release is touched;"
plan "a gh failure after that leaves the new drop in place"
printf '\n'

if [ "$DRY_RUN" -eq 1 ]; then
  note "dry run: nothing uploaded, copied or deleted"
  exit 0
fi
if [ "$CONFIRMED" -eq 0 ]; then
  note "plan only: re-run with --yes to carry it out"
  exit 0
fi

# ---------------------------------------------------------------------------
# Carry it out
# ---------------------------------------------------------------------------

# Asked here rather than at the release gate, so a dry run or a plan-only run
# never blocks on a prompt. --yes is a decision about the plan; this is a
# separate decision about the people holding the old asset hashes, so it is
# read from the terminal and cannot be pre-answered on the command line.
confirm_published_release() {
  local src reply src_dir tmp_real
  printf '\n' >&2
  warn "release ${TAG} is NOT a draft: it is published"
  warn "its assets are downloadable now, and anyone holding a SHA-256 of one will find it no longer matches"
  if [ -n "${HKV_PUBLISH_CONFIRM_FILE:-}" ]; then
    # Test-only hook: names a file the answer is read from, so the prompt can
    # be exercised without a terminal. Not for operator use. Confined to the
    # temp directory so a stray value in an operator environment cannot
    # pre-answer a real run. See #70.
    src="$HKV_PUBLISH_CONFIRM_FILE"
    [ -r "$src" ] || die "HKV_PUBLISH_CONFIRM_FILE is not readable: ${src}"
    src_dir="$(cd -P -- "$(dirname -- "$src")" 2>/dev/null && pwd)" \
      || die "confirmation file override is test-only"
    tmp_real="$(cd -P -- "${TMPDIR:-/tmp}" 2>/dev/null && pwd)" \
      || die "confirmation file override is test-only"
    case "${src_dir}/" in
      "${tmp_real}"/*) ;;
      *) die "confirmation file override is test-only" ;;
    esac
    src="${src_dir}/$(basename -- "$src")"
  else
    { [ -t 0 ] && [ -r /dev/tty ]; } \
      || die "--allow-published needs a terminal to confirm at; run it by hand rather than from a script or a pipe"
    src=/dev/tty
  fi
  printf 'type yes to replace the assets of published release %s: ' "$TAG" >&2
  read -r reply < "$src" \
    || die "no answer read from ${src}; leaving published release ${TAG} alone"
  printf '\n' >&2
  [ "$reply" = "yes" ] \
    || die "answer was not yes; leaving published release ${TAG} alone"
}

if [ "$RELEASE_STATE" = "published" ]; then
  confirm_published_release
fi

note "drop folder"

STAGING_LIVE=1
rm -rf "$STAGING"
mkdir -p "$STAGING"
cp -Rp "${PKG_ROOT}/." "$STAGING/"
cp -p "${ZIPS[@]}" "$STAGING/"
[ "$KEEP_RUNBOOK" -eq 1 ] && cp -p "${DEST}/FAE-RUNBOOK.md" "${STAGING}/FAE-RUNBOOK.md"

# Verified before the live folder is touched, so a bad copy costs nothing.
verify_sums "$STAGING" "$ALL_SUMS" "staging directory"

mv "$DEST" "$BACKUP"
mv "$STAGING" "$DEST"

verify_sums "$DEST" "$ALL_SUMS" "destination"

SWAP_VERIFIED=1
rm -rf "$BACKUP"

# The drop is correct from here on. A gh failure is reported against a
# destination that stays as it is: rolling it back would replace a verified
# drop with a stale one to match a release that was never updated. See #70.
gh_failed() {
  printf 'error: %s\n' "$1" >&2
  printf '%s\n' "the drop folder ${DEST} holds the new build and verified; it is left in place" >&2
  printf '%s\n' "the release ${TAG} was NOT updated; retry by hand:" >&2
  # Prefixed with the repository directory: gh infers the repository from the
  # working directory, and the asset and notes paths are repository relative,
  # so a bare command only works from where the run started.
  printf '  cd %q && gh release upload %q --clobber' "$REPO_DIR" "$TAG" >&2
  printf ' %q' "${ZIPS[@]}" >&2
  printf '\n' >&2
  printf '  cd %q && gh release edit %q -F %q\n' "$REPO_DIR" "$TAG" "$NOTES" >&2
  exit 1
}

note "release assets"
gh release upload "$TAG" --clobber "${ZIPS[@]}" \
  || gh_failed "gh release upload failed for ${TAG}"
gh release edit "$TAG" -F "$NOTES" \
  || gh_failed "gh release edit failed for ${TAG}"
gh release view "$TAG" --json assets --jq '.assets[] | "\(.name) \(.size)B"' || true

note "published ${TAG} to ${DEST}"
ls "$DEST"
