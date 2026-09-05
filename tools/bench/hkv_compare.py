#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Compare transport counters and signal quality across SWO captures.

Usage:
    python3 tools/bench/hkv_compare.py LOG [LOG ...]

For each log written by `swo_capture.py`, prints:

  - distributions (n, min, median, max) of ECG quality `qos`, model
    `cossim`, heart rate, CPU `util`, `cpu_nodemo` (measured minus demo
    transport) and `cpu_proj` (deployment projection) and `avg_ips`, plus
    TileIO `qdepth`;
  - the `tio` stream line: the cumulative `ecg_ok` counter at the first and
    last line of the capture, labelled as such, then ECG packets accepted as
    a per-second rate over that window, with `qdrop` and `ecg_fail` as
    deltas;
  - the `tiousb` line: `retry`, `drop` and `stall` as deltas over the whole
    window and over its second half, with drops normalised to per minute.

Apart from the two labelled `ecg_ok` endpoints, every cumulative firmware
counter is printed as a delta and a rate. The counters do not reset between
runs, so a total says nothing on its own; the endpoints are shown only to
make the window the deltas were taken over explicit. The second-half figures
exist because a defect that only starts once buffers fill is invisible in a
whole-window average. See #39.

Arguments:
    LOG         One or more capture logs from `swo_capture.py`.

Exit codes:
    0   Every log parsed and contained counter lines.
    1   A log was unreadable or contained no counter lines.
    2   Bad arguments (argparse).
"""

import argparse
import os
import re
import statistics
import sys

TS_RE = re.compile(r"^(\d+\.\d+) ")
ECGMET_RE = re.compile(
    r"ecgmet\|hr_x100=(-?\d+) hrv_x100=(-?\d+) qos_x100=(-?\d+) cossim_x100=(-?\d+)"
)
UTIL_RE = re.compile(r"util_x100=(-?\d+)")
# The three CPU figures are reported separately and must be read that way. See #8.
NODEMO_RE = re.compile(r"cpu_nodemo_x100=(-?\d+)")
PROJ_RE = re.compile(r"cpu_proj_x100=(-?\d+)")
IPS_RE = re.compile(r"avg_ips_x100=(-?\d+)")
TIO_RE = re.compile(r"tio\|.*?qdrop=(\d+).*?ecg_ok=(\d+).*?ecg_fail=(\d+).*?qdepth=(\d+)")
USB_RE = re.compile(r"tiousb\|ecg_retry=(\d+) ecg_drop=(\d+).*?stall=(\d+)")

DIST_KEYS = ["qos", "cossim", "hr", "util", "nodemo", "proj", "ips", "qdepth"]


def load(path):
    dist = {key: [] for key in DIST_KEYS}
    tio = []
    usb = []
    with open(path, errors="replace") as handle:
        for line in handle:
            stamp = TS_RE.match(line)
            timestamp = float(stamp.group(1)) if stamp else 0

            match = ECGMET_RE.search(line)
            if match:
                dist["hr"].append(int(match.group(1)) / 100)
                dist["qos"].append(int(match.group(3)) / 100)
                dist["cossim"].append(int(match.group(4)) / 100)

            match = UTIL_RE.search(line)
            if match:
                dist["util"].append(int(match.group(1)) / 100)

            match = NODEMO_RE.search(line)
            if match:
                dist["nodemo"].append(int(match.group(1)) / 100)

            match = PROJ_RE.search(line)
            if match:
                dist["proj"].append(int(match.group(1)) / 100)

            match = IPS_RE.search(line)
            if match:
                dist["ips"].append(int(match.group(1)) / 100)

            match = TIO_RE.search(line)
            if match:
                tio.append(
                    (
                        timestamp,
                        int(match.group(1)),
                        int(match.group(2)),
                        int(match.group(3)),
                        int(match.group(4)),
                    )
                )
                dist["qdepth"].append(int(match.group(4)))

            match = USB_RE.search(line)
            if match:
                usb.append(
                    (timestamp, int(match.group(1)), int(match.group(2)), int(match.group(3)))
                )
    return dist, tio, usb


def summarise(values):
    if not values:
        return "none"
    return (
        f"n={len(values)} min={min(values):.2f} "
        f"med={statistics.median(values):.2f} max={max(values):.2f}"
    )


def report(path):
    dist, tio, usb = load(path)
    print(f"=== {os.path.basename(path)}")
    for key in DIST_KEYS:
        print(f"  {key:7s} {summarise(dist[key])}")

    if tio:
        first, last = tio[0], tio[-1]
        window = last[0] - first[0] or 1
        print(
            f"  stream: ecg_ok cumulative {first[2]} -> {last[2]} "
            f"({(last[2] - first[2]) / window:.2f}/s)  qdrop +{last[1] - first[1]}  "
            f"ecg_fail +{last[3] - first[3]}  over {window:.0f}s"
        )

    if usb:
        first, last = usb[0], usb[-1]
        middle = usb[len(usb) // 2]
        window = last[0] - first[0] or 1
        half = last[0] - middle[0] or 1
        print(
            f"  usb full : retry +{last[1] - first[1]} drop +{last[2] - first[2]} "
            f"stall +{last[3] - first[3]} over {window:.0f}s  "
            f"-> drop {(last[2] - first[2]) / window * 60:.1f}/min"
        )
        print(
            f"  usb 2nd half: retry +{last[1] - middle[1]} drop +{last[2] - middle[2]} "
            f"stall +{last[3] - middle[3]} over {half:.0f}s "
            f"-> drop {(last[2] - middle[2]) / half * 60:.1f}/min"
        )
    return bool(tio or usb)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Compare transport counters and signal quality across SWO captures."
    )
    parser.add_argument("log", nargs="+", help="capture log from swo_capture.py")
    args = parser.parse_args(argv)

    ok = True
    for path in args.log:
        try:
            found = report(path)
        except OSError as exc:
            print(f"could not read {path}: {exc}", file=sys.stderr)
            ok = False
            continue
        if not found:
            print(f"no tio or tiousb counter lines in {path}", file=sys.stderr)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
