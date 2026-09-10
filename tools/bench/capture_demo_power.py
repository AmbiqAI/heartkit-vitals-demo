#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Capture one settled LP USB-streaming comparison for issue #68.

Requires an already-flashed demo and the JS110 on the MCU supply rail.
Writes raw capture, USB log and measured means to a new case directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

from pyjls import Reader
from lp_power_report import read_interval

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ai", choices=("on", "off"), required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--joulescope-serial", required=True)
    args = p.parse_args()
    if not re.fullmatch(r"[a-z0-9-]+", args.name):
        p.error("name must contain lowercase letters, digits or hyphens")
    out = ROOT / "tools/bench/results/active-recheck-20260909" / args.name
    out.mkdir(parents=True, exist_ok=False)
    state = "0600000000" + ("020202" if args.ai == "on" else "000000")
    with (out / "usb.log").open("w") as log:
        host = subprocess.Popen(["uv", "run", "--no-project", "--with", "pyusb",
            "python", "tools/tileio_usb_test.py", "--duration", "50",
            "--no-kick", "--uio-state", state, "--ppg-stats"], cwd=ROOT,
            stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(10)
            if host.poll() is not None:
                raise RuntimeError("USB host exited during settling")
            with (out / "record.log").open("w") as record_log:
                subprocess.run([sys.executable, "-m", "pyjoulescope_driver", "record",
                    "--serial_number", args.joulescope_serial, "--open", "restore", "--duration", "35",
                    "--frequency", "100000", "--set", "s/i/range/select=auto",
                    "--signals", "i,v,p", str(out / "capture.jls")], cwd=ROOT,
                    stdout=record_log, stderr=subprocess.STDOUT, check=True, timeout=45)
            host.wait(timeout=15)
            if host.returncode:
                raise RuntimeError("USB host failed")
        finally:
            if host.poll() is None:
                host.terminate()
                host.wait(timeout=5)
    log = (out / "usb.log").read_text()
    echoes = re.findall(r"uio echo: ([0-9a-f]+)", log)
    assert echoes and all(v == state for v in echoes), echoes
    summary = re.search(r"summary: packets=(\d+) bytes=(\d+) crc_errors=(\d+) duration=([\d.]+)s", log)
    assert summary and int(summary[1]) > 0 and int(summary[3]) == 0 and float(summary[4]) >= 49, log
    with Reader(str(out / "capture.jls")) as reader:
        signals = {s.name: s for s in reader.signals.values()}
        ref = signals["power"]
        start = reader.sample_id_to_timestamp(ref.signal_id, 5 * ref.sample_rate)
        stop = reader.sample_id_to_timestamp(ref.signal_id, 30 * ref.sample_rate)
        means = {n: float(read_interval(reader, signals[n], start, stop).mean())
                 for n in ("voltage", "current", "power")}
        assert means["current"] > 0 and means["power"] > 0, means
        blocks = []
        for sec in range(5, 30, 5):
            a = reader.sample_id_to_timestamp(ref.signal_id, sec * ref.sample_rate)
            b = reader.sample_id_to_timestamp(ref.signal_id, (sec + 5) * ref.sample_rate)
            blocks.append(float(read_interval(reader, ref, a, b).mean()))
    result = dict(ai=args.ai, uio_state=state, means_SI=means, block_power_W=blocks,
        analyzed_seconds=[5, 30], usb_summary=summary.group(0),
        sha256=hashlib.sha256((ROOT / "build/apollo510b_evb/heartkit-vitals-demo.bin").read_bytes()).hexdigest())
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
