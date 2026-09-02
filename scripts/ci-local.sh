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
#   scripts/ci-local.sh tests      # host tests only
#   scripts/ci-local.sh frozen     # frozen module sync check only
#   scripts/ci-local.sh build      # firmware build only
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BOARD="${BOARD:-apollo510b_evb}"
TARGET="${1:-all}"

run_host_tests() {
    echo "==> host unit tests (ASan + UBSan, -Werror)"
    cmake -S tests -B build/host
    cmake --build build/host
    ctest --test-dir build/host --output-on-failure
}

# Paths that `nsx sync` materialises, read out of nsx.lock so no module name is
# hard-coded here. `cmake/nsx` is appended by the caller: it is vendored too,
# but outside modules/.
lock_vendored_paths() {
    awk '$1 == "vendored_at:" && $2 ~ /^modules\// { print $2 }' nsx.lock | sort -u
}

# Every board target recorded in nsx.lock. Target names are the only keys at
# two-space indent inside the `targets:` block.
lock_boards() {
    awk '
        /^[A-Za-z_][A-Za-z0-9_]*:/ { in_targets = ($0 ~ /^targets:[[:space:]]*$/); next }
        in_targets && /^  [^ ][^:]*:[[:space:]]*$/ {
            sub(/^  /, ""); sub(/:[[:space:]]*$/, ""); print
        }
    ' nsx.lock
}

# Files under boards/ are vendored by `nsx sync` from packaged neuralspotx
# board modules and hash-checked against nsx.lock. Any local edit there makes
# `nsx sync --frozen` and `nsx configure --frozen` refuse, which breaks the
# release packaging path. Catch that drift here instead of at release time.
check_frozen_sync() {
    echo "==> frozen module sync check"
    # `modules/` alone proves nothing: `modules/.gitignore` is tracked, so a
    # fresh clone always has the directory and never the payload. Probe the
    # vendored paths recorded in nsx.lock instead.
    local path
    for path in $(lock_vendored_paths) cmake/nsx; do
        if [ ! -d "$path" ] || [ -z "$(ls -A "$path" 2>/dev/null)" ]; then
            echo "SKIPPED: frozen module sync check (${path} is not materialised)"
            return 0
        fi
    done
    if ! command -v uv >/dev/null 2>&1; then
        echo "SKIPPED: frozen module sync check (uv is not installed)"
        return 0
    fi
    uv run nsx sync --app-dir . --frozen
    # `nsx sync --frozen` validates the default lock target only, so an edit
    # under another board's directory passes it and then stops the release
    # build, which runs `nsx configure --frozen` per board. Check every target
    # here, each into its own build directory so the default build tree is left
    # alone.
    local board
    for board in $(lock_boards); do
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
    tests) run_host_tests ;;
    frozen) check_frozen_sync ;;
    build) run_firmware_build ;;
    all)
        run_host_tests
        check_frozen_sync
        run_firmware_build
        ;;
    *)
        echo "usage: $0 [all|tests|frozen|build]" >&2
        exit 2
        ;;
esac

echo "==> ci-local: OK"
