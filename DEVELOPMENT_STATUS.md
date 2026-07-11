# Development Status

## Current Validation

- `apollo510_evb`, `apollo510b_evb`, and `apollo330mP_evb` build from the
  same source tree.
- `apollo510b_evb` was programmed and flash-verified through J-Link.
- Apollo510B is the primary hardware target and supports USB and BLE TileIO.
- Apollo510 and Apollo330P support USB TileIO; validate hardware on the
  specific board before release.

## Verified Application Behavior

- AS7058 dual-wavelength PPG, ECG capture, AGC, pulse-rate, and SpO2 metrics.
- ECG DSP and AI denoise, segmentation, and arrhythmia pipelines.
- Canned patient playback, signal-noise controls, and host UIO mode updates.
- TileIO USB streaming; Apollo510B additionally supports BLE streaming.

## Open Engineering Work

- Review cross-task signaling and ring-buffer ownership in `src/main.cc`.
  The current design has no known hardware-observed failures, but a shared
  FreeRTOS event primitive would make the ownership model easier to audit.

## Hardware Notes

- Use J-Link SWD for flash and SWO output.
- USB TileIO uses `VID=0xCAFE` and `PID=0x0001`; connect the board USB data
  port separately from the J-Link debug connection.
- Run `python3 tools/tileio_usb_test.py --duration 5` after flashing to check
  TileIO packet flow. It should report `bad=0`.
