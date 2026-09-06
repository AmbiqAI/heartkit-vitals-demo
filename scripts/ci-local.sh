#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
#
# Local check runner: host unit tests, frozen module sync, then the firmware
# build.
#
# The host tests are a standalone CMake project under tests/ and deliberately
# do not use the NSX toolchain: `nsx` has no test subcommand and no host build
# mode, so unit tests are built with the host compiler instead.
#
# Usage:
#   scripts/ci-local.sh            # host tests + frozen sync + firmware build
#   scripts/ci-local.sh tests      # host tests and release helper tests only
#   scripts/ci-local.sh frozen     # frozen module sync check only
#   scripts/ci-local.sh build      # firmware build only
#
# CI_STRICT=1 turns every SKIPPED path into a failure (hosted CI sets it). See #6.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BOARD="${BOARD:-apollo510b_evb}"
TARGET="${1:-all}"
CI_STRICT="${CI_STRICT:-0}"

# Report a check that could not run. Returns 0 so the caller skips, or 1 under
# CI_STRICT=1 so the caller fails. The SKIPPED line is identical either way, so
# a strict failure and a local skip name the same cause.
skipped() {
    echo "SKIPPED: $1"
    if [ "$CI_STRICT" = "1" ]; then
        echo "ERROR: CI_STRICT=1, a skipped check is a failure" >&2
        return 1
    fi
    return 0
}

run_host_tests() {
    echo "==> host unit tests (ASan + UBSan, -Werror)"
    cmake -S tests -B build/host
    cmake --build build/host
    ctest --test-dir build/host --output-on-failure
}

# The release helpers are bash, not C, so they sit outside the CTest project.
# They run here because a publish helper that guards a shared drop folder is
# only as good as the tests for its guards. See #62.
run_release_tests() {
    echo "==> release helper tests"
    tools/release/test_publish.sh
}

# nsx.lock is YAML, so it is read with a YAML parser rather than by matching
# line shapes. A text match returns nothing when the writer changes its
# indentation, adds a sibling key or quotes a name, and an empty result would
# silently turn the per-board loop below into a no-op that still reports OK.
# PyYAML through `uv run python` is the same pattern as
# tools/release/verify_modules.py.
lock_query() {
    uv run python -c '
import sys

import yaml

with open("nsx.lock", encoding="utf-8") as fh:
    lock = yaml.safe_load(fh)

targets = (lock or {}).get("targets")
if not isinstance(targets, dict):
    sys.exit("nsx.lock has no targets mapping")

if sys.argv[1] == "boards":
    for name in targets:
        print(name)
else:
    paths = set()
    for target in targets.values():
        for module in ((target or {}).get("modules") or {}).values():
            at = ((module or {}).get("resolved") or {}).get("vendored_at")
            if isinstance(at, str) and at.startswith("modules/"):
                paths.add(at)
    for path in sorted(paths):
        print(path)
' "$1"
}

# Paths that `nsx sync` materialises, read out of nsx.lock so no module name is
# hard-coded here. `cmake/nsx` is appended by the caller: it is vendored too,
# but outside modules/.
lock_vendored_paths() {
    local paths
    if ! paths="$(lock_query paths)" || [ -z "$paths" ]; then
        echo "ERROR: no vendored paths parsed from nsx.lock" >&2
        return 1
    fi
    printf '%s\n' "$paths"
}

# Every board target recorded in nsx.lock.
lock_boards() {
    local boards
    if ! boards="$(lock_query boards)" || [ -z "$boards" ]; then
        echo "ERROR: no targets parsed from nsx.lock" >&2
        return 1
    fi
    printf '%s\n' "$boards"
}

# Files under boards/ are vendored by `nsx sync` from packaged neuralspotx
# board modules and hash-checked against nsx.lock. Any local edit there makes
# `nsx sync --frozen` and `nsx configure --frozen` refuse, which breaks the
# release packaging path. Catch that drift here instead of at release time.
check_frozen_sync() {
    echo "==> frozen module sync check"
    # Checked first: reading nsx.lock now also runs through `uv run python`.
    if ! command -v uv >/dev/null 2>&1; then
        skipped "frozen module sync check (uv is not installed)" || return 1
        return 0
    fi
    # `modules/` alone proves nothing: `modules/.gitignore` is tracked, so a
    # fresh clone always has the directory and never the payload. Probe the
    # vendored paths recorded in nsx.lock instead.
    local paths
    paths="$(lock_vendored_paths)" || return 1
    local path
    # Word splitting is intended: lock_vendored_paths prints one path per line.
    # shellcheck disable=SC2086
    for path in $paths cmake/nsx; do
        if [ ! -d "$path" ] || [ -z "$(ls -A "$path" 2>/dev/null)" ]; then
            skipped "frozen module sync check (${path} is not materialised)" \
                || return 1
            return 0
        fi
    done
    uv run nsx sync --app-dir . --frozen
    # `nsx sync --frozen` validates the default lock target only, so an edit
    # under another board's directory passes it and then stops the release
    # build, which runs `nsx configure --frozen` per board. Check every target
    # here, each into its own build directory so the default build tree is left
    # alone.
    local boards
    boards="$(lock_boards)" || return 1
    local board
    # Word splitting is intended: lock_boards prints one board name per line.
    # shellcheck disable=SC2086
    for board in $boards; do
        echo "==> frozen configure check (board=${board})"
        if ! uv run nsx configure --app-dir . --board "${board}" --frozen \
            --build-dir "build/frozen-check/${board}"; then
            echo "ERROR: frozen configure failed for board ${board}" >&2
            return 1
        fi
    done
}

run_firmware_build() {
    echo "==> firmware build (board=${BOARD})"
    uv run nsx build --app-dir . --board "${BOARD}"
}

case "$TARGET" in
    tests) run_host_tests; run_release_tests ;;
    frozen) check_frozen_sync ;;
    build) run_firmware_build ;;
    all)
        run_host_tests
        run_release_tests
        check_frozen_sync
        run_firmware_build
        ;;
    *)
        echo "usage: $0 [all|tests|frozen|build]" >&2
        exit 2
        ;;
esac

echo "==> ci-local: OK"
