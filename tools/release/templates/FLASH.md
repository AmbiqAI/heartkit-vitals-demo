# Flash the firmware

This package contains prebuilt firmware for the HeartKit Vitals demo. No
toolchain, no Python environment, and no source checkout are required.

Version: `@VERSION@`
Board: `@BOARD@` (folder `@BOARD_DIR@`)

## Prerequisites

- SEGGER J-Link software installed and on the system path. Download it from
  the SEGGER website and accept the default install location.
- The EVB connected to the host with the programming and debug USB cable, and
  the board powered on.

The WebUSB data cable is not needed for flashing. Attach it afterwards to run
the demo.

## Flash with a helper

1. Open the folder `@BOARD_DIR@`.
2. Run the helper for the host computer:
   - macOS: double-click `flash_mac.command`.
   - Windows: double-click `flash_win.bat`.
   - Linux: run `./flash_linux.sh` from a terminal, or launch it from a file
     manager configured to execute shell scripts.
3. Wait for the helper to report `Flash completed successfully.`

On macOS, the first run may be blocked by Gatekeeper. Any one of these clears
it:

- Right-click `flash_mac.command`, choose Open, then confirm.
- Remove the quarantine flag: `xattr -d com.apple.quarantine flash_mac.command`
- Run it from Terminal instead: `./flash_mac.command`

If `flash_linux.sh` will not run, `chmod +x flash_linux.sh` first.

## Manual fallback

Run J-Link Commander directly from inside the `@BOARD_DIR@` folder:

```
JLinkExe -nogui 1 -device @JLINK_DEVICE@ -if SWD -speed @SWD_SPEED@ -commandfile downloadfw.jlink
```

On Windows the binary is `JLink.exe`. The command file loads `firmware.bin` at
`@LOAD_ADDRESS@`, resets, and starts the target.

## Confirm

Attach the WebUSB data cable and confirm the board enumerates as
`@APP_NAME@` on the WebUSB port.

## Contents

| File | Purpose |
| --- | --- |
| `@BOARD_DIR@/firmware.bin` | Application image, loaded at `@LOAD_ADDRESS@`. |
| `@BOARD_DIR@/downloadfw.jlink` | J-Link command file used by all three helpers. |
| `@BOARD_DIR@/flash_mac.command` | One-click helper for macOS. |
| `@BOARD_DIR@/flash_win.bat` | One-click helper for Windows. |
| `@BOARD_DIR@/flash_linux.sh` | One-click helper for Linux. |
| `RELEASE.md` | Release notes. |
| `BUILD-INFO.txt` | Build provenance and the J-Link parameters with their sources. |
| `SHA256SUMS` | Checksums for every file above. |
| `@BOARD_DIR@-firmware.map` | Linker map, for debugging only. |

To verify the package, run `shasum -a 256 -c SHA256SUMS` on macOS or
`sha256sum -c SHA256SUMS` on Linux from the top of the extracted folder.
