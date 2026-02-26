#!/usr/bin/env python3
"""Generate AS7058 C profile artifacts from AMS JSON config."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path

SUPPORTED_STRUCT_VERSIONS = {3}

SECTIONS = {
    "power": {
        "type": "as7058_reg_group_power_t",
        "profile_field": "power",
        "fields": [
            "pwr_on",
            "pwr_iso",
            "clk_cfg",
            "ref_cfg1",
            "ref_cfg2",
            "ref_cfg3",
            "standby_on1",
            "standby_on2",
            "standby_en1",
            "standby_en2",
            "standby_en3",
            "standby_en4",
            "standby_en5",
            "standby_en6",
            "standby_en7",
            "standby_en8",
            "standby_en9",
            "standby_en10",
            "standby_en11",
            "standby_en12",
            "standby_en13",
            "standby_en14",
        ],
    },
    "control": {
        "type": "as7058_reg_group_control_t",
        "profile_field": "control",
        "fields": ["i2c_mode", "int_cfg", "if_cfg", "gpio_cfg1", "gpio_cfg2", "io_cfg"],
    },
    "led": {
        "type": "as7058_reg_group_led_t",
        "profile_field": "led",
        "fields": [
            "vcsel_password",
            "vcsel_cfg",
            "vcsel_mode",
            "led_cfg",
            "led_drv1",
            "led_drv2",
            "led1_ictrl",
            "led2_ictrl",
            "led3_ictrl",
            "led4_ictrl",
            "led5_ictrl",
            "led6_ictrl",
            "led7_ictrl",
            "led8_ictrl",
            "led_irng1",
            "led_irng2",
            "led_sub1",
            "led_sub2",
            "led_sub3",
            "led_sub4",
            "led_sub5",
            "led_sub6",
            "led_sub7",
            "led_sub8",
            "lowvds_wait",
        ],
    },
    "pd": {
        "type": "as7058_reg_group_pd_t",
        "profile_field": "pd",
        "fields": [
            "pdsel_cfg",
            "ppg1_pdsel1",
            "ppg1_pdsel2",
            "ppg1_pdsel3",
            "ppg1_pdsel4",
            "ppg1_pdsel5",
            "ppg1_pdsel6",
            "ppg1_pdsel7",
            "ppg1_pdsel8",
            "ppg2_pdsel1",
            "ppg2_pdsel2",
            "ppg2_pdsel3",
            "ppg2_pdsel4",
            "ppg2_pdsel5",
            "ppg2_pdsel6",
            "ppg2_pdsel7",
            "ppg2_pdsel8",
            "ppg2_afesel1",
            "ppg2_afesel2",
            "ppg2_afesel3",
            "ppg2_afesel4",
            "ppg2_afeen",
        ],
    },
    "ios": {
        "type": "as7058_reg_group_ios_t",
        "profile_field": "ios",
        "fields": [
            "ios_ppg1_sub1",
            "ios_ppg1_sub2",
            "ios_ppg1_sub3",
            "ios_ppg1_sub4",
            "ios_ppg1_sub5",
            "ios_ppg1_sub6",
            "ios_ppg1_sub7",
            "ios_ppg1_sub8",
            "ios_ppg2_sub1",
            "ios_ppg2_sub2",
            "ios_ppg2_sub3",
            "ios_ppg2_sub4",
            "ios_ppg2_sub5",
            "ios_ppg2_sub6",
            "ios_ppg2_sub7",
            "ios_ppg2_sub8",
            "ios_ledoff",
            "ios_cfg",
            "aoc_sar_thres",
            "aoc_sar_range",
            "aoc_sar_ppg1",
            "aoc_sar_ppg2",
        ],
    },
    "ppg": {
        "type": "as7058_reg_group_ppg_t",
        "profile_field": "ppg",
        "fields": [
            "ppgmod_cfg1",
            "ppgmod_cfg2",
            "ppgmod_cfg3",
            "ppgmod1_cfg1",
            "ppgmod1_cfg2",
            "ppgmod1_cfg3",
            "ppgmod2_cfg1",
            "ppgmod2_cfg2",
            "ppgmod2_cfg3",
        ],
    },
    "ecg": {
        "type": "as7058_reg_group_ecg_t",
        "profile_field": "ecg",
        "fields": [
            "bioz_cfg",
            "bioz_excit",
            "bioz_mixer",
            "bioz_select",
            "bioz_gain",
            "ecgmod_cfg1",
            "ecgmod_cfg2",
            "ecgimux_cfg1",
            "ecgimux_cfg2",
            "ecgimux_cfg3",
            "ecgamp_cfg1",
            "ecgamp_cfg2",
            "ecgamp_cfg3",
            "ecgamp_cfg4",
            "ecgamp_cfg5",
            "ecgamp_cfg6",
            "ecgamp_cfg7",
            "ecg_bioz",
            "leadoff_cfg",
            "leadoff_thresl",
            "leadoff_thresh",
        ],
    },
    "sinc": {
        "type": "as7058_reg_group_sinc_t",
        "profile_field": "sinc",
        "fields": [
            "ppg_sinc_cfga",
            "ppg_sinc_cfgb",
            "ppg_sinc_cfgc",
            "ppg_sinc_cfgd",
            "ecg1_sinc_cfga",
            "ecg1_sinc_cfgb",
            "ecg1_sinc_cfgc",
            "ecg2_sinc_cfga",
            "ecg2_sinc_cfgb",
            "ecg2_sinc_cfgc",
            "ecg_sinc_cfg",
        ],
    },
    "seq": {
        "type": "as7058_reg_group_seq_t",
        "profile_field": "seq",
        "fields": [
            "irq_enable",
            "ppg_sub_wait",
            "ppg_sar_wait",
            "ppg_led_init",
            "ppg_freql",
            "ppg_freqh",
            "ppg1_sub_en",
            "ppg2_sub_en",
            "ppg_mode_1",
            "ppg_mode_2",
            "ppg_mode_3",
            "ppg_mode_4",
            "ppg_mode_5",
            "ppg_mode_6",
            "ppg_mode_7",
            "ppg_mode_8",
            "ppg_cfg",
            "ecg_freql",
            "ecg_freqh",
            "ecg1_freqdivl",
            "ecg1_freqdivh",
            "ecg2_freqdivl",
            "ecg2_freqdivh",
            "ecg_subs",
            "leadoff_initl",
            "leadoff_inith",
            "ecg_initl",
            "ecg_inith",
            "sample_num",
        ],
    },
    "post": {
        "type": "as7058_reg_group_pp_t",
        "profile_field": "pp",
        "fields": ["pp_cfg", "ppg1_pp1", "ppg1_pp2", "ppg2_pp1", "ppg2_pp2"],
    },
    "fifo": {
        "type": "as7058_reg_group_fifo_t",
        "profile_field": "fifo",
        "fields": ["fifo_threshold", "fifo_ctrl"],
    },
}

IIR_FIELDS = ["iir_cfg", "iir_coeff_data_sos"]

BOARD_CAPABILITIES = {
    "any": {"allowed_led_mask": 0xFF, "allowed_pd_mask": 0xFF},
    "click": {"allowed_led_mask": 0x07, "allowed_pd_mask": 0x16},
    "evk": {"allowed_led_mask": 0x77, "allowed_pd_mask": 0x16},
}


def _require(obj: dict, key: str, path: str):
    if key not in obj:
        raise ValueError(f"Missing key {path}.{key}")
    return obj[key]


def _validate_u8(value: int, path: str) -> int:
    if not isinstance(value, int):
        raise ValueError(f"{path} must be integer")
    if value < 0 or value > 0xFF:
        raise ValueError(f"{path} out of uint8 range: {value}")
    return value


def _validate_i16(value: int, path: str) -> int:
    if not isinstance(value, int):
        raise ValueError(f"{path} must be integer")
    if value < -32768 or value > 32767:
        raise ValueError(f"{path} out of int16 range: {value}")
    return value


def _validate_u16(value: int, path: str) -> int:
    if not isinstance(value, int):
        raise ValueError(f"{path} must be integer")
    if value < 0 or value > 0xFFFF:
        raise ValueError(f"{path} out of uint16 range: {value}")
    return value


def _validate_bool01(value, path: str) -> int:
    if isinstance(value, bool):
        return 1 if value else 0
    if isinstance(value, int) and value in (0, 1):
        return value
    raise ValueError(f"{path} must be bool or 0/1 integer")


def _render_section(section_key: str, section_values: dict) -> list[str]:
    spec = SECTIONS[section_key]
    lines = [f"    .{spec['profile_field']} = {{{{" ]
    for field_name in spec["fields"]:
        value = _validate_u8(_require(section_values, field_name, f"sensor.{section_key}"),
                             f"sensor.{section_key}.{field_name}")
        lines.append(f"        .{field_name} = {value},")
    lines.append("    }},")
    return lines


def _parse_iir(sensor: dict) -> dict:
    iir_section = sensor.get("iir")
    if iir_section is not None and not isinstance(iir_section, dict):
        raise ValueError("device_as7058.sensor.iir must be an object when present")

    iir_present = 1 if iir_section is not None else 0
    iir_enabled = 0
    iir_enabled_implicit_default = 0
    iir_cfg_val = 0
    coeffs_validated: list[list[int]] = []

    if iir_section is None:
        return {
            "present": iir_present,
            "enabled": iir_enabled,
            "enabled_implicit_default": iir_enabled_implicit_default,
            "iir_cfg": iir_cfg_val,
            "coeffs": coeffs_validated,
        }

    if "iir_enabled" in sensor:
        iir_enabled = _validate_bool01(sensor["iir_enabled"], "device_as7058.sensor.iir_enabled")
    else:
        iir_enabled = 0
        iir_enabled_implicit_default = 1

    iir_cfg_val = _validate_u8(_require(iir_section, "iir_cfg", "sensor.iir"), "sensor.iir.iir_cfg")
    coeffs = _require(iir_section, "iir_coeff_data_sos", "sensor.iir")
    if not isinstance(coeffs, list):
        raise ValueError("sensor.iir.iir_coeff_data_sos must be an array")
    if len(coeffs) > 12:
        raise ValueError(f"sensor.iir.iir_coeff_data_sos supports max 12 rows, got {len(coeffs)}")

    for row_idx, row in enumerate(coeffs):
        if not isinstance(row, list) or len(row) != 5:
            raise ValueError(f"sensor.iir.iir_coeff_data_sos[{row_idx}] must be length-5 array")
        validated = [_validate_i16(v, f"sensor.iir.iir_coeff_data_sos[{row_idx}]") for v in row]
        coeffs_validated.append(validated)

    return {
        "present": iir_present,
        "enabled": iir_enabled,
        "enabled_implicit_default": iir_enabled_implicit_default,
        "iir_cfg": iir_cfg_val,
        "coeffs": coeffs_validated,
    }


def _render_iir_struct(iir_info: dict) -> list[str]:
    lines = ["    .iir = {{"]
    if not iir_info["present"]:
        lines.extend(
            [
                "        .iir_cfg = 0,",
                "        .reserved = 0,",
                "        .iir_coeff_data_sos = {{0}},",
            ]
        )
    else:
        lines.append(f"        .iir_cfg = {iir_info['iir_cfg']},")
        lines.append("        .reserved = 0,")
        lines.append("        .iir_coeff_data_sos = {")
        for row in iir_info["coeffs"]:
            row_text = ", ".join(str(v) for v in row)
            lines.append(f"            {{{row_text}}},")
        lines.append("        },")
    lines.append("    }},")
    lines.append(f"    .iir_present = {iir_info['present']},")
    lines.append(f"    .iir_enabled = {iir_info['enabled']},")
    return lines


def _parse_spo2(device: dict) -> dict:
    applications = device.get("applications")
    if applications is None:
        return {
            "present": 0,
            "enabled": 0,
            "routing": {"ppg_red": 0, "ppg_ir": 0, "ambient_light": 0},
            "config": {"a": 0, "b": 0, "c": 0, "dc_comp_red": 0, "dc_comp_ir": 0},
        }
    if not isinstance(applications, dict):
        raise ValueError("device_as7058.applications must be an object when present")

    spo2 = applications.get("spo2")
    if spo2 is None:
        return {
            "present": 0,
            "enabled": 0,
            "routing": {"ppg_red": 0, "ppg_ir": 0, "ambient_light": 0},
            "config": {"a": 0, "b": 0, "c": 0, "dc_comp_red": 0, "dc_comp_ir": 0},
        }
    if not isinstance(spo2, dict):
        raise ValueError("device_as7058.applications.spo2 must be an object when present")

    enabled = _validate_bool01(_require(spo2, "enabled", "device_as7058.applications.spo2"),
                               "device_as7058.applications.spo2.enabled")
    params = _require(spo2, "parameters", "device_as7058.applications.spo2")
    if not isinstance(params, dict):
        raise ValueError("device_as7058.applications.spo2.parameters must be an object")
    routing = _require(spo2, "signal_routing", "device_as7058.applications.spo2")
    if not isinstance(routing, dict):
        raise ValueError("device_as7058.applications.spo2.signal_routing must be an object")

    return {
        "present": 1,
        "enabled": enabled,
        "routing": {
            "ppg_red": _validate_u8(_require(routing, "ppg_red", "device_as7058.applications.spo2.signal_routing"),
                                    "device_as7058.applications.spo2.signal_routing.ppg_red"),
            "ppg_ir": _validate_u8(_require(routing, "ppg_ir", "device_as7058.applications.spo2.signal_routing"),
                                   "device_as7058.applications.spo2.signal_routing.ppg_ir"),
            "ambient_light": _validate_u8(_require(routing, "ambient_light", "device_as7058.applications.spo2.signal_routing"),
                                          "device_as7058.applications.spo2.signal_routing.ambient_light"),
        },
        "config": {
            "a": _validate_u16(_require(params, "cal_coeff_a", "device_as7058.applications.spo2.parameters"),
                               "device_as7058.applications.spo2.parameters.cal_coeff_a"),
            "b": _validate_u16(_require(params, "cal_coeff_b", "device_as7058.applications.spo2.parameters"),
                               "device_as7058.applications.spo2.parameters.cal_coeff_b"),
            "c": _validate_u16(_require(params, "cal_coeff_c", "device_as7058.applications.spo2.parameters"),
                               "device_as7058.applications.spo2.parameters.cal_coeff_c"),
            "dc_comp_red": _validate_u16(_require(params, "dc_comp_red", "device_as7058.applications.spo2.parameters"),
                                         "device_as7058.applications.spo2.parameters.dc_comp_red"),
            "dc_comp_ir": _validate_u16(_require(params, "dc_comp_ir", "device_as7058.applications.spo2.parameters"),
                                        "device_as7058.applications.spo2.parameters.dc_comp_ir"),
        },
    }


def _render_spo2_struct(spo2_info: dict) -> list[str]:
    lines = [
        f"    .spo2_present = {spo2_info['present']},",
        f"    .spo2_enabled = {spo2_info['enabled']},",
        f"    .spo2_red_sub_sample = {spo2_info['routing']['ppg_red']},",
        f"    .spo2_ir_sub_sample = {spo2_info['routing']['ppg_ir']},",
        f"    .spo2_ambient_sub_sample = {spo2_info['routing']['ambient_light']},",
        "    .spo2_config = {",
        f"        .a = {spo2_info['config']['a']},",
        f"        .b = {spo2_info['config']['b']},",
        f"        .c = {spo2_info['config']['c']},",
        f"        .dc_comp_red = {spo2_info['config']['dc_comp_red']},",
        f"        .dc_comp_ir = {spo2_info['config']['dc_comp_ir']},",
        "    },",
    ]
    return lines


def _board_validate(sensor: dict, board: str) -> None:
    caps = BOARD_CAPABILITIES[board]
    allowed_led_mask = caps["allowed_led_mask"]
    allowed_pd_mask = caps["allowed_pd_mask"]

    led = sensor["led"]
    led_sub_mask = 0
    for key in ("led_sub1", "led_sub2", "led_sub3", "led_sub4", "led_sub5", "led_sub6", "led_sub7", "led_sub8"):
        led_sub_mask |= _validate_u8(_require(led, key, "device_as7058.sensor.led"), f"device_as7058.sensor.led.{key}")

    led_ictrl_mask = 0
    for idx in range(1, 9):
        key = f"led{idx}_ictrl"
        value = _validate_u8(_require(led, key, "device_as7058.sensor.led"), f"device_as7058.sensor.led.{key}")
        if value:
            led_ictrl_mask |= (1 << (idx - 1))

    invalid_led_mask = (led_sub_mask | led_ictrl_mask) & (~allowed_led_mask & 0xFF)
    if invalid_led_mask:
        raise ValueError(
            f"Board '{board}' does not support LED mask bits 0x{invalid_led_mask:02X} "
            f"(allowed 0x{allowed_led_mask:02X})"
        )

    pd = sensor["pd"]
    invalid_pd_mask = 0
    for prefix in ("ppg1_pdsel", "ppg2_pdsel"):
        for idx in range(1, 9):
            key = f"{prefix}{idx}"
            value = _validate_u8(_require(pd, key, "device_as7058.sensor.pd"), f"device_as7058.sensor.pd.{key}")
            invalid_pd_mask |= value & (~allowed_pd_mask & 0xFF)
    if invalid_pd_mask:
        raise ValueError(
            f"Board '{board}' does not support PD mask bits 0x{invalid_pd_mask:02X} "
            f"(allowed 0x{allowed_pd_mask:02X})"
        )


def generate(json_path: Path, profile_name: str, out_header: Path, board: str) -> tuple[Path, Path]:
    raw = json_path.read_bytes()
    data = json.loads(raw)

    device = _require(data, "device_as7058", "root")
    struct_version = _require(device, "struct_version", "device_as7058")
    if struct_version not in SUPPORTED_STRUCT_VERSIONS:
        raise ValueError(
            f"Unsupported device_as7058.struct_version={struct_version}. Supported={sorted(SUPPORTED_STRUCT_VERSIONS)}"
        )

    sensor = _require(device, "sensor", "device_as7058")

    for section in SECTIONS:
        if section not in sensor:
            raise ValueError(f"Missing required section device_as7058.sensor.{section}")

    iir_info = _parse_iir(sensor)
    spo2_info = _parse_spo2(device)

    if board not in BOARD_CAPABILITIES:
        raise ValueError(f"Unsupported board '{board}'. Supported: {sorted(BOARD_CAPABILITIES.keys())}")
    _board_validate(sensor, board)

    source_hash = hashlib.sha256(raw).hexdigest()
    timestamp = dt.datetime.fromtimestamp(json_path.stat().st_mtime, tz=dt.timezone.utc).isoformat()

    out_header.parent.mkdir(parents=True, exist_ok=True)
    out_source = out_header.with_suffix(".c")

    guard = "__" + out_header.stem.upper() + "_H__"

    header_text = "\n".join(
        [
            f"#ifndef {guard}",
            f"#define {guard}",
            "",
            f"/* Generated from {json_path.as_posix()} */",
            f"/* Source SHA256: {source_hash} */",
            f"/* Source mtime (UTC): {timestamp} */",
            f"/* Board validation: {board} */",
            "",
            '#include "../as7058_profiles.h"',
            "",
            f"extern const as7058_sensor_profile_t g_as7058_profile_{profile_name};",
            "",
            f"#endif /* {guard} */",
            "",
        ]
    )

    body_lines: list[str] = [
        f"/* Generated from {json_path.as_posix()} */",
        f"/* Source SHA256: {source_hash} */",
        f"/* Source mtime (UTC): {timestamp} */",
        f"/* Board validation: {board} */",
        "",
        f'#include "{out_header.name}"',
        "",
    ]

    if iir_info["present"] and iir_info["enabled_implicit_default"]:
        body_lines.append(
            "/* warning: sensor.iir exists but sensor.iir_enabled missing in source JSON; defaulted to disabled. */"
        )
        body_lines.append("")

    body_lines.append(f"const as7058_sensor_profile_t g_as7058_profile_{profile_name} = {{")

    for section in SECTIONS:
        body_lines.extend(_render_section(section, sensor[section]))

    body_lines.extend(_render_iir_struct(iir_info))
    body_lines.extend(_render_spo2_struct(spo2_info))

    body_lines.extend(
        [
            "    .agc_config = {0},",
            "    .agc_config_num = 0,",
            "};",
            "",
        ]
    )

    out_header.write_text(header_text)
    out_source.write_text("\n".join(body_lines))

    return out_header, out_source


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate AS7058 sensor profile C artifacts from AMS JSON")
    parser.add_argument("--json", required=True, help="Path to AMS JSON profile")
    parser.add_argument("--name", required=True, help="Symbol suffix name, e.g. click_ppg_ecg")
    parser.add_argument("--out", required=True, help="Output header path (e.g. src/generated/as7058_profile_click_ppg_ecg.h)")
    parser.add_argument(
        "--board",
        default="any",
        choices=sorted(BOARD_CAPABILITIES.keys()),
        help="Board capabilities to validate against (any|click|evk)",
    )
    args = parser.parse_args()

    json_path = Path(args.json)
    out_header = Path(args.out)

    if not json_path.exists():
        raise FileNotFoundError(f"JSON file not found: {json_path}")
    if out_header.suffix != ".h":
        raise ValueError("--out must point to a .h file")

    out_h, out_c = generate(json_path, args.name, out_header, args.board)
    print(f"Generated: {out_h}")
    print(f"Generated: {out_c}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
