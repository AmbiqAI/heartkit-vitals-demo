#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
set -euo pipefail

elf="${1:?usage: check-aot-only.sh <firmware ELF>}"
nm_tool="${NM:-}"
cache="$(dirname "$elf")/CMakeCache.txt"
if [ -z "$nm_tool" ] && [ -f "$cache" ]; then
    nm_tool="$(sed -n 's/^CMAKE_NM:FILEPATH=//p' "$cache")"
fi
nm_tool="${nm_tool:-arm-none-eabi-nm}"
symbols="$("$nm_tool" -C "$elf")"
if grep -Eq 'tflite::|tflm_|MicroInterpreter|helia_rt' <<< "$symbols"; then
    echo "ERROR: interpreter symbols remain in $elf" >&2
    exit 1
fi
for model in denoise segmentation arrhythmia; do
    if ! grep -Eq "[[:space:]]hkv_${model}_model_run$" <<< "$symbols"; then
        echo "ERROR: missing AOT entry point for $model" >&2
        exit 1
    fi
done
echo "PASS: three AOT entry points and no TFLM/heliaRT symbols in $elf"
