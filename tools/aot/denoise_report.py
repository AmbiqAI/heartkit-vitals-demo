#!/usr/bin/env python3
"""Validate repeated denoise parity telemetry from an AP510B capture."""
import argparse
import re
from pathlib import Path


def validate(text):
    cases = {}
    summaries = {}
    for line in text.splitlines():
        match = re.search(r"HKV\|parity\|(den|den_summary) (.*)", line)
        if not match:
            continue
        fields = dict(item.split("=", 1) for item in match[2].split() if "=" in item)
        mode = fields["mode"]
        if mode not in ("lp", "hp") or fields["ref"] != "tflm":
            raise ValueError("Unexpected mode or reference")
        if match[1] == "den_summary":
            if fields["pass"] != "8/8" or fields["ref_init"] != "0":
                raise ValueError(f"Failed summary: {fields}")
            summaries[mode] = fields
            continue
        key = (mode, int(fields["case"]))
        if fields["rc"] != "0" or fields["finite"] != "1" or fields["pass"] != "1":
            raise ValueError(f"Failed case: {fields}")
        if int(fields["cycles"]) <= 0 or int(fields["ref_cycles"]) <= 0:
            raise ValueError("Missing execution evidence")
        if key in cases and cases[key] != fields:
            raise ValueError(f"Conflicting repeated case: {key}")
        cases[key] = fields
    if set(cases) != {(mode, case) for mode in ("lp", "hp") for case in range(8)}:
        raise ValueError("Incomplete case coverage")
    if set(summaries) != {"lp", "hp"}:
        raise ValueError("Missing summaries")
    return max(int(row["max_abs_nano"]) for row in cases.values())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    try:
        worst = validate(args.capture.read_text())
    except (ValueError, KeyError) as error:
        parser.exit(1, f"FAIL: {error}\n")
    print(f"PASS: 16/16 denoise cases; finite outputs; worst absolute error approximately {worst / 1e9:.9g}")
