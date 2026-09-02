#!/bin/bash
cd -- "$(dirname "$0")"

if command -v JLinkExe >/dev/null 2>&1; then
  jlink="$(command -v JLinkExe)"
elif [ -x /Applications/SEGGER/JLink/JLinkExe ]; then
  jlink=/Applications/SEGGER/JLink/JLinkExe
else
  echo "SEGGER J-Link was not found. Install it, then run this helper again."
  read -r -p "Press Return to close..."
  exit 1
fi

"$jlink" -nogui 1 -device @JLINK_DEVICE@ -if SWD -speed @SWD_SPEED@ -commandfile downloadfw.jlink
status=$?
[ "$status" -eq 0 ] && echo "Flash completed successfully."
[ "$status" -ne 0 ] && echo "Flash failed (exit code $status)."
read -r -p "Press Return to close..."
exit "$status"
