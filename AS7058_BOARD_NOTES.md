# AS7058 EVK vs Click Board Notes

## Scope
This note captures current wiring assumptions and firmware mapping differences between:
- AS7058A EVK board
- AS7058 click module

## Device-Level Difference
- EVK reference stack is built around **AS7058A** usage (supports bundled "A" algorithm binaries used in this repo).
- Click module is assumed to use **AS7058** (non-A), so bundled SpO2/RRM "A" algorithms are currently disabled for click bring-up.

## Transport
- EVK profile: SPI
- Click profile: I2C (`0x55`, `100 kHz` default)

## Schematic Wiring Summary
- EVK:
  - LED1 + LED5: Green
  - LED2 + LED6: Red
  - LED3 + LED7: IR
  - PD2, PD3, PD5 connected
- Click:
  - LED1: Green
  - LED2: Red
  - LED3: IR
  - PD2, PD3, PD5 connected (same SFH7074)

## Current Firmware Mapping (PPG path)
- Active PPG sub-samples: `PPG1_SUB1`, `PPG1_SUB2`, `PPG1_SUB3` (`ppg1_sub_en = 7`)
- Data processing currently uses:
  - `PPG1_SUB1` as Red
  - `PPG1_SUB2` as IR
  - `PPG1_SUB3` not used for metrics

## LED Mapping in Firmware
- EVK profile:
  - `SUB1 -> LED2 + LED6` (`AS7058_LED_SUB1_CFG = 34`)
  - `SUB2 -> LED3 + LED7` (`AS7058_LED_SUB2_CFG = 51`)
- Click profile:
  - `SUB1 -> LED2` (`AS7058_LED_SUB1_CFG = 0x02`)
  - `SUB2 -> LED3` (`AS7058_LED_SUB2_CFG = 0x03`)

## PD Mapping in Firmware
- Current PD selection is still:
  - `PPG1_PDSEL1 = 0x02`
  - `PPG1_PDSEL2 = 0x02`
  - `PPG1_PDSEL3 = 0x02`
- `0x02` selects **PD2** only (bitmask format: bit0=PD1 ... bit7=PD8).
- PD3/PD5 are physically present but not currently enabled in firmware.

## Bring-Up Status
- Bring-up mode is intended to validate reliable register reads first.
- Full signal-quality tuning (LED current/PD blend/sequence) is deferred until communication is stable.
