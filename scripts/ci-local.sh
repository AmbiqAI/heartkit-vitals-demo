#!/usr/bin/env bash
#
# Local check runner: host unit tests, then the firmware build.
#
# The host tests are a standalone CMake project under tests/ and deliberately
# do not use the NSX toolchain: `nsx` has no test subcommand and no host build
# mode, so unit tests are built with the host compiler instead.
#
# Usage:
#   scripts/ci-local.sh            # host tests + firmware build
#   scripts/ci-local.sh tests      # host tests only
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

run_firmware_build() {
    echo "==> firmware build (board=${BOARD})"
    uv run nsx build --app-dir . --board "${BOARD}"
}

case "$TARGET" in
    tests) run_host_tests ;;
    build) run_firmware_build ;;
    all)
        run_host_tests
        run_firmware_build
        ;;
    *)
        echo "usage: $0 [all|tests|build]" >&2
        exit 2
        ;;
esac

echo "==> ci-local: OK"
