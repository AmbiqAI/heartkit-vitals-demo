#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Report settled firmware metrics from an SWO capture.

Usage:
    python3 tools/bench/hkv_analyze.py LOG [LOG ...] [--label LABEL]
                                       [--skip N]

Parses the `HKV|<uptime_ms>|<seq>|<subsystem>|k=v` lines emitted by
`src/obs.c` out of a log written by `swo_capture.py`, and reports:

  - how many HKV lines the capture contains;
  - firmware uptime against host wall clock, as a ratio (1.0 is nominal, a
    ratio far from 1.0 means the timebase or the capture is wrong);
  - settled means of `util_x100`, `batt_days_x100`, `batt_inf_x100`,
    `batt_pwr_x100` and `avg_ips_x100` from the `cpu` subsystem, each
    divided by 100 and printed with the sample count;
  - the same for `cpu_proj_x100`, which is stated against the measured
    `util_x100` above and never blended with it, and for the per-component
    breakdown -- see `docs/developer.md`;
  - settled mean and maximum of the per-model inference latencies
    `den_lat_us`, `seg_lat_us` and `arr_lat_us` and their `*_max_us`
    counterparts, in microseconds and unscaled;
  - the per-model TFLM arena used and configured sizes, printed once;
  - attempted ECG, PPG and CPU packet rates from the `tio` subsystem, as a
    rate over the window rather than a raw total.

The first `--skip` samples are discarded before the means, so start-up
transients do not move the numbers.

Arguments:
    LOG         One or more capture logs from `swo_capture.py`.
    --label     Tag printed in front of every line. Default: empty.
    --skip      Samples dropped before the settled means. Default: 40.

Exit codes:
    0   Every log parsed and contained HKV lines.
    1   A log was unreadable or contained no HKV lines.
    2   Bad arguments (argparse).
"""

import argparse
import re
import sys

LINE_RE = re.compile(r"^(\d+\.\d+) HKV\|(\d+)\|\d+\|(\w+)\|(.*)$")
MEAN_KEYS = [
    "util_x100",
    "batt_days_x100",
    "batt_inf_x100",
    "batt_pwr_x100",
    "avg_ips_x100",
    # Projected CPU, then the breakdown. The measured figure is util_x100
    # above; other and idle are derivable and not emitted. See #8.
    "cpu_proj_x100",
    "cpu_cap_x100",
    "cpu_inf_x100",
    "cpu_tx_x100",
]
# Plain microseconds, not hundredths, so these take their own mean/max pass.
# Baseline for the TFLM/AOT comparison. See #37.
LATENCY_KEYS = [
    "den_lat_us",
    "seg_lat_us",
    "arr_lat_us",
    "den_lat_max_us",
    "seg_lat_max_us",
    "arr_lat_max_us",
]
ARENA_KEYS = [
    "den_arena_used",
    "den_arena_size",
    "seg_arena_used",
    "seg_arena_size",
    "arr_arena_used",
    "arr_arena_size",
]
PACKET_PREFIXES = ["ecg", "ppg", "cpu"]

# tio counters are cumulative, so rates come from a pair of lines about 30 s apart. See #39.
RATE_WINDOW_LINES = 31


def load(path):
    rows = []
    with open(path, errors="replace") as handle:
        for line in handle:
            match = LINE_RE.match(line.strip())
            if match:
                rows.append(
                    (
                        float(match.group(1)),
                        int(match.group(2)),
                        match.group(3),
                        match.group(4),
                    )
                )
    return rows


def _values(rows, key, subsystem, skip):
    return [
        int(value)
        for row in rows
        if row[2] == subsystem
        for value in re.findall(key + r"=(\d+)", row[3])
    ][skip:]


def settled_mean(rows, key, subsystem, skip):
    values = _values(rows, key, subsystem, skip)
    if not values:
        return None, 0
    return round(sum(values) / len(values) / 100, 2), len(values)


def settled_mean_max(rows, key, subsystem, skip):
    """Mean, max and count of an unscaled integer key."""
    values = _values(rows, key, subsystem, skip)
    if not values:
        return None, None, 0
    return round(sum(values) / len(values), 1), max(values), len(values)


def first_value(rows, key):
    for row in rows:
        found = re.findall(key + r"=(\d+)", row[3])
        if found:
            return int(found[0])
    return None


def report(path, label, skip):
    rows = load(path)
    print(f"[{label}] HKV lines: {len(rows)}")
    if not rows:
        print(f"no HKV lines in {path}", file=sys.stderr)
        return False

    cpu = [row for row in rows if row[2] == "cpu"]
    if len(cpu) > 12:
        first, last = cpu[10], cpu[-1]
        d_uptime = last[1] - first[1]
        # Lines sharing one host millisecond collapse the window to zero. See #39.
        d_wall = (last[0] - first[0]) * 1000 or 1
        if d_wall == 1:
            print(
                f"[{label}] host timestamps span no time; the ratio below is "
                "against a substituted 1 ms window and means nothing",
                file=sys.stderr,
            )
        print(
            f"[{label}] uptime_ms vs wall clock: d_uptime={d_uptime} ms "
            f"d_wall={d_wall:.0f} ms ratio={d_uptime / d_wall:.4f}"
        )

    for key in MEAN_KEYS:
        print(f"[{label}] {key:16s} {settled_mean(rows, key, 'cpu', skip)}")

    for key in LATENCY_KEYS:
        print(f"[{label}] {key:16s} {settled_mean_max(rows, key, 'cpu', skip)}")

    arena = [(key, first_value(rows, key)) for key in ARENA_KEYS]
    for key, value in arena:
        if value is not None:
            print(f"[{label}] {key:16s} {value} bytes")

    tio = [row for row in rows if row[2] == "tio"]
    if len(tio) > 2:
        print(f"[{label}] tio keys:", tio[-1][3][:260])
        first = tio[max(0, len(tio) - RATE_WINDOW_LINES)]
        last = tio[-1]
        window = last[0] - first[0] or 1
        if window == 1:
            print(
                f"[{label}] tio lines span no time; the rates below are against "
                "a substituted 1 s window and mean nothing",
                file=sys.stderr,
            )
        start = dict(re.findall(r"(\w+)=(\d+)", first[3]))
        end = dict(re.findall(r"(\w+)=(\d+)", last[3]))
        for prefix in PACKET_PREFIXES:
            try:
                attempted = (int(end[prefix + "_ok"]) + int(end[prefix + "_fail"])) - (
                    int(start[prefix + "_ok"]) + int(start[prefix + "_fail"])
                )
            except KeyError:
                continue
            print(
                f"[{label}] attempted {prefix} packets: {attempted / window:.2f}/s "
                f"over {window:.0f}s (ok+fail; fail expected with no host)"
            )
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Report settled firmware metrics from an SWO capture."
    )
    parser.add_argument("log", nargs="+", help="capture log from swo_capture.py")
    parser.add_argument("--label", default="", help="tag printed on every line")
    parser.add_argument(
        "--skip", type=int, default=40, help="samples dropped before the means (default: 40)"
    )
    args = parser.parse_args(argv)

    if args.skip < 0:
        parser.error("skip must not be negative")

    ok = True
    for path in args.log:
        try:
            ok = report(path, args.label, args.skip) and ok
        except OSError as exc:
            print(f"could not read {path}: {exc}", file=sys.stderr)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
