#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
set -e

cd -- "$(dirname "$0")"

if command -v JLinkExe >/dev/null 2>&1; then
  jlink="$(command -v JLinkExe)"
elif [ -x /opt/SEGGER/JLink/JLinkExe ]; then
  jlink=/opt/SEGGER/JLink/JLinkExe
else
  echo "SEGGER J-Link was not found. Install it, then run this helper again." >&2
  exit 1
fi

"$jlink" -nogui 1 -device @JLINK_DEVICE@ -if SWD -speed @SWD_SPEED@ -commandfile downloadfw.jlink
echo "Flash completed successfully."
