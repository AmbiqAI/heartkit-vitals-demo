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

# Files under boards/ are vendored by `nsx sync` from packaged neuralspotx
# board modules and hash-checked against nsx.lock. Any local edit there makes
# `nsx sync --frozen` and `nsx configure --frozen` refuse, which breaks the
# release packaging path. Catch that drift here instead of at release time.
check_frozen_sync() {
    echo "==> frozen module sync check"
    if [ ! -d modules ]; then
        echo "SKIPPED: frozen module sync check (modules/ is not materialised)"
        return 0
    fi
    if ! command -v uv >/dev/null 2>&1; then
        echo "SKIPPED: frozen module sync check (uv is not installed)"
        return 0
    fi
    uv run nsx sync --app-dir . --frozen
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
