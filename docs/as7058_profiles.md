# AS7058 Profiles

## Overview
This repo uses compile-time AS7058 sensor profiles to keep `sensor_configure()` small and make profile selection explicit.

`src/as7058_profiles.c` contains:
- legacy/default profile (`g_as7058_profile_legacy_default`) matching prior hardcoded values.
- profile selection via `as7058_get_active_profile()`.
- deterministic apply order via `as7058_apply_sensor_profile()`.
- firmware-side board sanity validation before applying a profile (rejects unsupported LED/PD usage).

`src/sensor.c` now:
1. gets active profile,
2. copies to mutable local profile,
3. applies board/transport overrides,
4. applies profile to hardware.

## Profile Selection
In `src/constants.h`:
- `AS7058_APP_PROFILE_LEGACY_DEFAULT`
- `AS7058_APP_PROFILE_CLICK_PPG_ECG`
- `AS7058_APP_PROFILE_CLICK_SPO2`
- `AS7058_APP_PROFILE_CLICK_GOLDEN`
- `AS7058_APP_PROFILE` (default: `AS7058_APP_PROFILE_CLICK_GOLDEN`)

Board profile selection remains independent (`AS7058_BOARD_PROFILE`), and currently defaults to `AS7058_PROFILE_CLICK_I2C`.

## Override Precedence
`sensor_configure()` applies these overrides after loading profile:
- `control.i2c_mode = AS7058_USE_I2C ? 1 : 0`
- if SpO2 profile is disabled, force:
  - `led.led_sub1 = AS7058_LED_SUB1_CFG`
  - `led.led_sub2 = AS7058_LED_SUB2_CFG`

This prevents imported profiles from breaking transport/wiring assumptions.

## Current Bring-Up/Tuning Defaults
Current `src/constants.h` defaults used for click bring-up:
- `PPG_AGC_MIN = 180000`
- `PPG_AGC_MAX = 620000`
- `PPG_TX_GAIN = 1.0f`
- `EN_SPO2_AGC_EVENT_TRACE = 0`
- `EN_SPO2_RAW_STATS_TRACE = 0`
- `EN_SPO2_AGC_VERIFY_TRACE = 0`

PPG TX path (`send_ppg_signals`) centers around the active AGC span midpoint derived from `PPG_AGC_MIN/MAX` before int16 packing.

## JSON Generator
Tool: `tools/as7058_json_to_profile.py`

Example:
```bash
tools/as7058_json_to_profile.py \
  --json assets/Life_metrics_Click_PPG-ECG.json \
  --board click \
  --name click_ppg_ecg \
  --out src/generated/as7058_profile_click_ppg_ecg.h
```

This emits:
- `src/generated/as7058_profile_click_ppg_ecg.h`
- `src/generated/as7058_profile_click_ppg_ecg.c`

## JSON Mapping
Generator maps `device_as7058.sensor` sections as follows:
- `power -> profile.power`
- `control -> profile.control`
- `led -> profile.led`
- `pd -> profile.pd`
- `ios -> profile.ios`
- `ppg -> profile.ppg`
- `ecg -> profile.ecg`
- `sinc -> profile.sinc`
- `seq -> profile.seq`
- `post -> profile.pp`
- `fifo -> profile.fifo`

Generator also maps optional SpO2 app config from `device_as7058.applications.spo2`:
- `enabled -> profile.spo2_enabled` (with `profile.spo2_present`)
- `signal_routing.ppg_red -> profile.spo2_red_sub_sample`
- `signal_routing.ppg_ir -> profile.spo2_ir_sub_sample`
- `signal_routing.ambient_light -> profile.spo2_ambient_sub_sample`
- `parameters.cal_coeff_a/b/c + dc_comp_* -> profile.spo2_config`

`iir` and `iir_enabled` are parsed and emitted into profile fields:
- `profile.iir`
- `profile.iir_present`
- `profile.iir_enabled`

### IIR Dual Gate
IIR register group (`AS7058_REG_GROUP_ID_IIR`) is applied only when all are true:
- `profile.iir_present == 1`
- `profile.iir_enabled == 1` (from JSON `sensor.iir_enabled`; defaults to `0` if omitted)
- `EN_AS7058_IIR == 1` (compile-time gate in `src/constants.h`, default `0`)

Enable at build time (example):
```bash
make DEFINES+=EN_AS7058_IIR=1
```

## Validation Rules
Generator validates:
- `device_as7058.struct_version` is supported.
- required sections exist.
- register fields fit expected integer ranges.
- `iir_coeff_data_sos` shape and int16 ranges.
- board constraints when `--board` is set:
  - `click`: LEDs limited to mask `0x07` (LED1..LED3), PDs limited to `0x16` (PD2/PD3/PD5)
  - `evk`: LEDs limited to mask `0x77` (LED1/2/3/5/6/7), PDs limited to `0x16` (PD2/PD3/PD5)

## Board Default JSON Profiles
This repo includes one-time default board profiles in JSON form:
- `assets/default_evk_spi_sensor_profile.json`
- `assets/default_click_i2c_sensor_profile.json`
- `assets/Life_metrics_Click_SpO2.json` (example SpO2-enabled app profile)

Generate from these defaults:
```bash
tools/as7058_json_to_profile.py \
  --json assets/default_evk_spi_sensor_profile.json \
  --board evk \
  --name default_evk_spi \
  --out src/generated/as7058_profile_default_evk_spi.h

tools/as7058_json_to_profile.py \
  --json assets/default_click_i2c_sensor_profile.json \
  --board click \
  --name default_click_i2c \
  --out src/generated/as7058_profile_default_click_i2c.h
```
