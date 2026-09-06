#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
#
# Host tests for tools/release/publish.sh. No network and no GitHub: a fake
# `gh` on PATH answers the release queries and records every call, so the
# assertions can say what was and was not invoked.
#
#   tools/release/test_publish.sh
#
# Each test builds its own fixture: a temporary HOME with a drop folder two
# levels below it, a repository with dist/<slug>/ and its archives, and a
# destination holding a stale file plus an FAE-RUNBOOK.md that must survive.
#
# See #62.

set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PUBLISH="${SCRIPT_DIR}/publish.sh"
[ -x "$PUBLISH" ] || { printf 'error: %s is not executable\n' "$PUBLISH" >&2; exit 1; }

REAL_SHASUM="$(command -v shasum)" || { printf 'error: shasum not on PATH\n' >&2; exit 1; }

PASSED=0
FAILED=0
FIXTURES=()

cleanup() {
  local f
  for f in ${FIXTURES[@]+"${FIXTURES[@]}"}; do
    [ -n "$f" ] && [ -d "$f" ] && rm -rf "$f"
  done
}
trap cleanup EXIT

ok()  { printf '  ok   %s\n' "$1"; PASSED=$((PASSED + 1)); }
bad() { printf '  FAIL %s\n' "$1"; FAILED=$((FAILED + 1)); }

dump() {
  printf '       --- stdout ---\n'; sed 's/^/       /' "${ROOT}/out" | tail -30
  printf '       --- stderr ---\n'; sed 's/^/       /' "${ROOT}/err" | tail -30
}

assert_rc() {
  # assert_rc <want> <name>
  if [ "$RC" -eq "$1" ]; then ok "$2"; else bad "$2 (rc=${RC}, wanted $1)"; dump; fi
}

assert_has() {
  # assert_has <file> <pattern> <name>
  if grep -qF -- "$2" "$1"; then ok "$3"; else bad "$3 (no '$2' in $(basename "$1"))"; dump; fi
}

assert_lacks() {
  # assert_lacks <file> <pattern> <name>
  if grep -qF -- "$2" "$1"; then bad "$3 (unexpected '$2')"; dump; else ok "$3"; fi
}

assert_content() {
  # assert_content <file> <want> <name>
  local got
  got="$(cat "$1" 2>/dev/null)"
  if [ "$got" = "$2" ]; then ok "$3"; else bad "$3 (got '${got}', wanted '$2')"; fi
}

assert_absent() {
  if [ -e "$1" ]; then bad "$2 ($1 exists)"; else ok "$2"; fi
}

assert_exists() {
  if [ -e "$1" ]; then ok "$2"; else bad "$2 ($1 missing)"; fi
}

# Snapshot of everything in the destination, so "untouched" is a byte claim
# rather than a file listing.
dest_manifest() {
  ( cd "$DEST" && find . -type f -print0 | LC_ALL=C sort -z \
      | xargs -0 "$REAL_SHASUM" -a 256 )
}

# fake_gh <isDraft>  ("true" or "false")
fake_gh() {
  cat > "${BIN}/gh" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "${ROOT}/gh.log"
case "\$*" in
  *"--json isDraft"*) printf '%s\n' '$1' ;;
  *"--json assets"*)  printf 'fixture-asset.zip 12B\n' ;;
esac
exit 0
EOF
  chmod +x "${BIN}/gh"
}

# fake_shasum <n>: pass every call through to the real shasum, except the nth
# `-c` check, which fails. Injecting the failure from PATH keeps the test hook
# out of the script under test.
fake_shasum() {
  cat > "${BIN}/shasum" <<EOF
#!/usr/bin/env bash
is_check=0
for a in "\$@"; do [ "\$a" = "-c" ] && is_check=1; done
if [ "\$is_check" -eq 1 ]; then
  n=\$(cat "${ROOT}/shasum.count" 2>/dev/null || printf 0)
  n=\$((n + 1))
  printf '%s' "\$n" > "${ROOT}/shasum.count"
  if [ "\$n" -eq $1 ]; then
    printf 'injected checksum failure\n' >&2
    exit 1
  fi
fi
exec "${REAL_SHASUM}" "\$@"
EOF
  chmod +x "${BIN}/shasum"
}

fixture() {
  # An explicit template: -t with a bare prefix is a BSD extension that GNU
  # coreutils rejects. A failure here must stop the run, not leave ROOT empty
  # and let the fixture write into the current directory. See #70.
  ROOT="$(mktemp -d "${TMPDIR:-/tmp}/hkv-publish-test.XXXXXX")" \
    || { printf 'error: mktemp failed\n' >&2; exit 1; }
  [ -n "$ROOT" ] && [ -d "$ROOT" ] \
    || { printf 'error: fixture root was not created\n' >&2; exit 1; }
  FIXTURES+=("$ROOT")
  FAKE_HOME="${ROOT}/home"
  REPO="${ROOT}/repo"
  BIN="${ROOT}/bin"
  DEST="${FAKE_HOME}/share/firmware/v500"
  PKG="${REPO}/dist/v500"

  mkdir -p "$BIN" "$DEST" "${PKG}/apollo510b"

  printf 'release notes\n' > "${PKG}/RELEASE.md"
  printf 'flash instructions\n' > "${PKG}/FLASH.md"
  printf 'firmware image\n' > "${PKG}/apollo510b/firmware.bin"
  # A map still naming the build machine: the path scan must skip maps, so this
  # file failing the run would be a regression.
  printf 'ld map referencing /Users/packager/build\n' > "${PKG}/apollo510b-firmware.map"
  ( cd "$PKG" && find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z \
      | xargs -0 "$REAL_SHASUM" -a 256 > SHA256SUMS )

  printf 'per-board archive\n' > "${REPO}/dist/heartkit-vitals-demo-v500-apollo510b-firmware.zip"
  printf 'combined archive\n' > "${REPO}/dist/heartkit-vitals-demo-v500-firmware.zip"
  printf '# release notes\n' > "${REPO}/notes.md"

  printf 'runbook the field team owns\n' > "${DEST}/FAE-RUNBOOK.md"
  printf 'stale file from the previous drop\n' > "${DEST}/STALE.txt"

  # Created up front so "gh was not called" is an assertion about an empty log
  # rather than about a missing file.
  : > "${ROOT}/gh.log"
  fake_gh true
  ARGS=(--tag v5.0.0 --notes notes.md --dest "$DEST" --repo-dir "$REPO")
  BEFORE="$(dest_manifest)"
}

# publish <args...>: run the script under the fixture's HOME and PATH.
publish() {
  ( cd "$REPO" && HOME="$FAKE_HOME" PATH="${BIN}:${PATH}" "$PUBLISH" "$@" ) \
    > "${ROOT}/out" 2> "${ROOT}/err"
  RC=$?
}


# ---------------------------------------------------------------------------

test_dry_run_prints_plan_and_changes_nothing() {
  printf '== dry run prints a plan and changes nothing\n'
  fixture
  # --yes as well, to prove --dry-run wins over it.
  publish --tag v5.0.0 --notes notes.md --dest "$DEST" --repo-dir "$REPO" --dry-run --yes
  assert_rc 0 "exits 0"
  assert_has "${ROOT}/out" "publish plan for v5.0.0" "prints the plan header"
  assert_has "${ROOT}/out" "assets to upload" "lists the assets to upload"
  assert_has "${ROOT}/out" "heartkit-vitals-demo-v500-firmware.zip" "names the archives"
  assert_has "${ROOT}/out" "files to copy" "lists the files to copy"
  assert_has "${ROOT}/out" "apollo510b/firmware.bin" "names a copied file"
  assert_has "${ROOT}/out" "files to delete" "lists the files to delete"
  assert_has "${ROOT}/out" "STALE.txt" "names the stale file"
  assert_has "${ROOT}/out" "checksums to verify" "says what it would verify"
  assert_has "${ROOT}/out" "dry run: nothing uploaded" "says it did nothing"
  assert_lacks "${ROOT}/gh.log" "release upload" "no gh release upload"
  assert_lacks "${ROOT}/gh.log" "release edit" "no gh release edit"
  if [ "$(dest_manifest)" = "$BEFORE" ]; then
    ok "destination is byte identical"
  else
    bad "destination is byte identical"
  fi
  assert_absent "${FAKE_HOME}/share/firmware/.v500.staging" "no staging directory left"
}

test_plan_only_without_yes() {
  printf '== without --yes the plan is printed and nothing happens\n'
  fixture
  publish "${ARGS[@]}"
  assert_rc 0 "exits 0"
  assert_has "${ROOT}/out" "re-run with --yes" "tells the operator how to proceed"
  assert_lacks "${ROOT}/gh.log" "release upload" "no gh release upload"
  if [ "$(dest_manifest)" = "$BEFORE" ]; then
    ok "destination is byte identical"
  else
    bad "destination is byte identical"
  fi
}

test_destination_guard() {
  printf '== destination guard\n'
  fixture

  publish --tag v5.0.0 --notes notes.md --dest "$FAKE_HOME" --repo-dir "$REPO" --yes
  assert_rc 1 "refuses \$HOME itself"
  assert_has "${ROOT}/err" 'refusing to replace $HOME' "says why it refused \$HOME"

  mkdir -p "${FAKE_HOME}/v500"
  publish --tag v5.0.0 --notes notes.md --dest "${FAKE_HOME}/v500" --repo-dir "$REPO" --yes
  assert_rc 1 "refuses a path one level below \$HOME"
  assert_has "${ROOT}/err" "at least two levels" "says why it refused one level"

  publish --tag v5.0.0 --notes notes.md --dest "${DEST}/" --repo-dir "$REPO" --yes
  assert_rc 1 "refuses a trailing slash"
  assert_has "${ROOT}/err" "must not end in a slash" "says why it refused the slash"

  publish --tag v5.0.0 --notes notes.md --dest "${FAKE_HOME}/share/nope/v500" \
    --repo-dir "$REPO" --yes
  assert_rc 1 "refuses a path with a missing component"
  assert_has "${ROOT}/err" "missing component" "says why it refused the missing path"

  mkdir -p "${ROOT}/outside/v500"
  publish --tag v5.0.0 --notes notes.md --dest "${ROOT}/outside/v500" \
    --repo-dir "$REPO" --yes
  assert_rc 1 "refuses a path outside \$HOME"
  assert_has "${ROOT}/err" 'outside $HOME' "says why it refused the outside path"

  mkdir -p "${FAKE_HOME}/share/firmware/v510"
  publish --tag v5.0.0 --notes notes.md --dest "${FAKE_HOME}/share/firmware/v510" \
    --repo-dir "$REPO" --yes
  assert_rc 1 "refuses a folder not named for the slug"
  assert_has "${ROOT}/err" "must be named v500" "says which name it wanted"

  assert_lacks "${ROOT}/gh.log" "release upload" "no gh release upload from a rejected run"
  if [ "$(dest_manifest)" = "$BEFORE" ]; then
    ok "destination is byte identical"
  else
    bad "destination is byte identical"
  fi
}

test_draft_gate() {
  printf '== draft gate\n'
  fixture
  fake_gh false

  publish "${ARGS[@]}" --yes
  assert_rc 1 "refuses a published release"
  assert_has "${ROOT}/err" "already published" "says the release is published"
  assert_lacks "${ROOT}/gh.log" "release upload" "no gh release upload"
  if [ "$(dest_manifest)" = "$BEFORE" ]; then
    ok "destination is byte identical"
  else
    bad "destination is byte identical"
  fi

  publish "${ARGS[@]}" --allow-published --dry-run
  assert_rc 0 "--allow-published gets past the gate"
  assert_has "${ROOT}/out" "(published)" "the plan records the release state"
}

test_swap_publishes_and_preserves_runbook() {
  printf '== the swap publishes and preserves the runbook\n'
  fixture
  publish "${ARGS[@]}" --yes
  assert_rc 0 "exits 0"
  assert_has "${ROOT}/gh.log" "release upload" "uploaded the assets"
  assert_has "${ROOT}/gh.log" "release edit" "set the release notes"
  assert_exists "${DEST}/RELEASE.md" "copied the drop files"
  assert_exists "${DEST}/apollo510b/firmware.bin" "copied the board folder"
  assert_exists "${DEST}/heartkit-vitals-demo-v500-firmware.zip" "copied the archives"
  assert_content "${DEST}/FAE-RUNBOOK.md" "runbook the field team owns" "preserved the runbook"
  assert_absent "${DEST}/STALE.txt" "removed the stale file"
  assert_absent "${FAKE_HOME}/share/firmware/.v500.backup" "removed the backup"
  assert_absent "${FAKE_HOME}/share/firmware/.v500.staging" "removed the staging directory"
}

test_rollback_restores_backup() {
  printf '== an injected failure after the swap restores the backup\n'
  fixture
  # Third check is the destination, after the live folder has been replaced.
  fake_shasum 3
  publish "${ARGS[@]}" --yes
  assert_rc 1 "exits 1"
  assert_has "${ROOT}/err" "restoring the previous destination" "reports the restore"
  assert_content "${DEST}/FAE-RUNBOOK.md" "runbook the field team owns" "runbook survived"
  assert_content "${DEST}/STALE.txt" "stale file from the previous drop" "old drop survived"
  assert_absent "${DEST}/RELEASE.md" "the new drop is not left in place"
  assert_absent "${FAKE_HOME}/share/firmware/.v500.backup" "no backup left behind"
  assert_absent "${FAKE_HOME}/share/firmware/.v500.staging" "no staging left behind"
  if [ "$(dest_manifest)" = "$BEFORE" ]; then
    ok "destination is byte identical to before the run"
  else
    bad "destination is byte identical to before the run"
  fi
}

test_checksum_mismatch_fails() {
  printf '== a checksum mismatch stops the run\n'
  fixture
  printf 'tampered after SHA256SUMS was written\n' > "${PKG}/RELEASE.md"
  publish "${ARGS[@]}" --yes
  assert_rc 1 "exits 1"
  assert_has "${ROOT}/err" "checksums do not verify" "says the checksums failed"
  assert_lacks "${ROOT}/gh.log" "release upload" "nothing was uploaded"
  if [ "$(dest_manifest)" = "$BEFORE" ]; then
    ok "destination is byte identical"
  else
    bad "destination is byte identical"
  fi
}

test_missing_sha256sums_fails() {
  printf '== a missing SHA256SUMS stops the run\n'
  fixture
  rm -f "${PKG}/SHA256SUMS"
  publish "${ARGS[@]}" --yes
  assert_rc 1 "exits 1"
  assert_has "${ROOT}/err" "SHA256SUMS is missing" "says the checksum file is missing"
  assert_lacks "${ROOT}/gh.log" "release upload" "nothing was uploaded"
}

test_leftover_backup_stops_before_gh() {
  printf '== a leftover backup stops the run before anything is uploaded\n'
  fixture
  mkdir -p "${FAKE_HOME}/share/firmware/.v500.backup"
  printf 'the destination an earlier run could not restore\n' \
    > "${FAKE_HOME}/share/firmware/.v500.backup/RELEASE.md"
  publish "${ARGS[@]}" --yes
  assert_rc 1 "exits 1"
  assert_has "${ROOT}/err" "backup from an earlier run" "names the leftover backup"
  assert_lacks "${ROOT}/out" "publish plan" "stops before the plan, so before any gh call"
  assert_lacks "${ROOT}/gh.log" "release upload" "nothing was uploaded"
  assert_lacks "${ROOT}/gh.log" "release edit" "the notes were not set"
  assert_exists "${FAKE_HOME}/share/firmware/.v500.backup/RELEASE.md" "the leftover backup is untouched"
  if [ "$(dest_manifest)" = "$BEFORE" ]; then
    ok "destination is byte identical"
  else
    bad "destination is byte identical"
  fi
}

test_rollback_reports_a_failed_restore() {
  printf '== a restore that cannot be carried out is reported, not claimed\n'
  fixture
  # cp -Rp carries the mode across, so after the swap the live folder holds a
  # directory whose contents cannot be removed and the restore has nowhere to
  # land.
  chmod 500 "${PKG}/apollo510b"
  fake_shasum 3
  publish "${ARGS[@]}" --yes
  assert_rc 1 "exits 1"
  assert_has "${ROOT}/err" "COULD NOT RESTORE" "reports the failed restore"
  assert_lacks "${ROOT}/err" "destination restored" "does not claim a restore that did not happen"
  assert_absent "${DEST}/.v500.backup" "the backup is not nested inside the destination"
  assert_content "${FAKE_HOME}/share/firmware/.v500.backup/FAE-RUNBOOK.md" \
    "runbook the field team owns" "the previous destination survives in the backup"
  chmod -R u+rwx "$ROOT" 2>/dev/null
}

test_archive_matching_is_exact() {
  printf '== only the archives named for this slug are uploaded\n'
  fixture
  printf 'a release candidate drop\n' \
    > "${REPO}/dist/heartkit-vitals-demo-v500-rc1-firmware.zip"
  printf 'a board that is not in the drop\n' \
    > "${REPO}/dist/heartkit-vitals-demo-v500-apollo330-firmware.zip"
  publish "${ARGS[@]}" --dry-run
  assert_rc 0 "exits 0"
  assert_has "${ROOT}/out" "heartkit-vitals-demo-v500-firmware.zip" "keeps the combined archive"
  assert_has "${ROOT}/out" "heartkit-vitals-demo-v500-apollo510b-firmware.zip" "keeps the per-board archive"
  assert_lacks "${ROOT}/out" "v500-rc1-firmware.zip" "ignores a suffixed slug"
  assert_lacks "${ROOT}/out" "v500-apollo330-firmware.zip" "ignores a board absent from the drop"
}

test_unreadable_draft_state_fails() {
  printf '== an unreadable draft state stops the run\n'
  fixture
  cat > "${BIN}/gh" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "${ROOT}/gh.log"
case "\$*" in
  *"--json isDraft"*) printf 'gh: release not found\n' >&2; exit 1 ;;
esac
exit 0
EOF
  chmod +x "${BIN}/gh"
  publish "${ARGS[@]}" --yes
  assert_rc 1 "exits 1 when gh errors"
  assert_has "${ROOT}/err" "could not read the draft state" "says the state is unknown"
  assert_lacks "${ROOT}/gh.log" "release upload" "nothing was uploaded"

  # gh exiting 0 with nothing to say is the same stop.
  cat > "${BIN}/gh" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "${ROOT}/gh.log"
exit 0
EOF
  chmod +x "${BIN}/gh"
  publish "${ARGS[@]}" --yes
  assert_rc 1 "exits 1 on an empty answer"
  assert_has "${ROOT}/err" "could not read the draft state" "says the state is unknown"
  assert_lacks "${ROOT}/gh.log" "release upload" "still nothing uploaded"
  if [ "$(dest_manifest)" = "$BEFORE" ]; then
    ok "destination is byte identical"
  else
    bad "destination is byte identical"
  fi
}

printf '==> tools/release/publish.sh\n'
test_dry_run_prints_plan_and_changes_nothing
test_plan_only_without_yes
test_destination_guard
test_draft_gate
test_swap_publishes_and_preserves_runbook
test_rollback_restores_backup
test_checksum_mismatch_fails
test_missing_sha256sums_fails
test_leftover_backup_stops_before_gh
test_rollback_reports_a_failed_restore
test_archive_matching_is_exact
test_unreadable_draft_state_fails

printf '\n%s passed, %s failed\n' "$PASSED" "$FAILED"
[ "$FAILED" -eq 0 ] || exit 1
printf '==> publish.sh tests: OK\n'
