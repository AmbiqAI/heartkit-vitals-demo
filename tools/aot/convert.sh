#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
#
# Regenerate the committed heliaAOT modules from assets/*.tflite.
#
#   tools/aot/convert.sh            # regenerate modules/hkv_*_aot and re-lock
#   tools/aot/convert.sh --check    # regenerate to a temp dir, fail on drift
#
# The converter refuses to overwrite an existing output tree, so each run
# removes the destination first. The only nondeterminism between runs is the
# generation timestamp in the file banners, which --check ignores.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

HELIA_AOT_VERSION="0.19.0"
PYTHON_VERSION="3.12"
MODELS="segmentation arrhythmia"

aot_convert() {
    # $1 = yaml config, $2 = output parent directory
    uv tool run --from "helia-aot==${HELIA_AOT_VERSION}" --python "${PYTHON_VERSION}" \
        helia-aot convert --path "$1" --module.path "$2"
}

module_name() {
    case "$1" in
        segmentation) echo "hkv_segmentation_aot" ;;
        arrhythmia) echo "hkv_arrhythmia_aot" ;;
        *) echo "unknown model: $1" >&2; return 1 ;;
    esac
}

regenerate() {
    for model in $MODELS; do
        name="$(module_name "$model")"
        rm -rf "modules/${name}"
        aot_convert "tools/aot/${model}.yaml" modules
    done
    uv run nsx lock --app-dir .
}

check() {
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/hkv-aot-check.XXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    status=0
    for model in $MODELS; do
        name="$(module_name "$model")"
        aot_convert "tools/aot/${model}.yaml" "$tmp"
        if ! diff -r -I '^.*[Gg]enerated by heliaAOT.*$' -I '^.*@date.*$' \
                "modules/${name}" "${tmp}/${name}"; then
            echo "ERROR: modules/${name} differs from a fresh conversion" >&2
            status=1
        fi
    done
    if [ "$status" -eq 0 ]; then
        echo "OK: committed AOT modules match a fresh conversion"
    fi
    return "$status"
}

case "${1:-}" in
    --check) check ;;
    "") regenerate ;;
    *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac
