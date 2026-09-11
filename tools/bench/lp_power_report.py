#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Summarize GPIO-gated JS110 JLS captures from lp_power.c.

Requires pyjls and numpy. Rejects incomplete sequences and nonfinite samples.
The output CSV contains measured rail power, not a product-runtime claim.
"""
import argparse
import csv
from pathlib import Path
import numpy as np
from pyjls import Reader

PHASES = ("spin", "denoise", "segment", "arrhythmia", "light_sleep")


def high_windows(reader, signal):
    """Return complete high intervals in the gate signal's sample domain."""
    state, start, windows = 0, None, []
    block = 1_000_000
    for offset in range(0, signal.length, block):
        n = min(block, signal.length - offset)
        bits = np.unpackbits(reader.fsr(signal.signal_id, offset, n), bitorder="little")[:n]
        transitions = np.flatnonzero(np.diff(np.r_[state, bits].astype(np.int8)))
        for local in transitions:
            index = offset + int(local)
            if bits[local]:
                start = index if index else None
            elif start is not None:
                windows.append((start, index))
                start = None
        state = int(bits[-1])
    return windows


def read_interval(reader, signal, t0, t1):
    lo = int(round(reader.timestamp_to_sample_id(signal.signal_id, t0)))
    hi = int(round(reader.timestamp_to_sample_id(signal.signal_id, t1)))
    if lo < 0 or hi > signal.length or hi <= lo:
        raise ValueError(f"Invalid interval for {signal.name}: {lo}:{hi}")
    values = reader.fsr(signal.signal_id, lo, hi - lo).astype(np.float64)
    if not np.all(np.isfinite(values)):
        raise ValueError(f"Missing/nonfinite samples in {signal.name}")
    return values


def deglitch(windows, max_samples):
    merged = []
    for lo, hi in windows:
        if merged and lo - merged[-1][1] <= max_samples:
            merged[-1] = (merged[-1][0], hi)
        else:
            merged.append((lo, hi))
    return [(lo, hi) for lo, hi in merged if hi - lo > max_samples]


def summarize(path, deglitch_us=0):
    with Reader(str(path)) as reader:
        signals = {s.name: s for s in reader.signals.values()}
        gate = signals["gpi[0]"]
        windows = high_windows(reader, gate)
        if deglitch_us:
            windows = deglitch(windows, int(deglitch_us * gate.sample_rate / 1_000_000))
        candidates = []
        for idx, (lo, hi) in enumerate(windows):
            duration = (hi - lo) / gate.sample_rate
            if 0.08 < duration < 0.12:
                candidates.append(idx)
        if len(candidates) != 1:
            raise ValueError(f"Expected one 100 ms preamble, found {len(candidates)}")
        windows = windows[candidates[0] + 1:]
        if len(windows) != 15:
            raise ValueError(f"Expected 15 windows, found {len(windows)}")
        rows = []
        for idx, (lo, hi) in enumerate(windows):
            t0 = reader.sample_id_to_timestamp(gate.signal_id, lo)
            t1 = reader.sample_id_to_timestamp(gate.signal_id, hi)
            duration = (hi - lo) / gate.sample_rate
            if not 2.8 < duration < 3.3:
                raise ValueError(f"Window {idx} duration out of bounds: {duration}")
            current = read_interval(reader, signals["current"], t0, t1)
            voltage = read_interval(reader, signals["voltage"], t0, t1)
            power = read_interval(reader, signals["power"], t0, t1)
            trim = int(0.01 * signals["power"].sample_rate)
            rows.append(dict(
                repeat=idx // len(PHASES), phase=PHASES[idx % len(PHASES)],
                duration_s=duration, voltage_V=float(voltage.mean()),
                current_mA=float(current.mean() * 1000),
                power_mW=float(power.mean() * 1000),
                steady_power_mW=float(power[trim:-trim].mean() * 1000),
                energy_mJ=float(power.sum() / signals["power"].sample_rate * 1000),
            ))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--deglitch-us", type=int, default=0, choices=range(0, 201),
                        metavar="0..200", help="Explicit gate-only glitch limit; default is strict")
    args = parser.parse_args()
    rows = summarize(args.capture, args.deglitch_us)
    if args.deglitch_us:
        print(f"Gate deglitching enabled: {args.deglitch_us} us; analog samples unchanged")
    with args.csv.open("x", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=rows[0].keys(), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    for phase in PHASES:
        selected = [row for row in rows if row["phase"] == phase]
        powers = [row["power_mW"] for row in selected]
        currents = [row["current_mA"] for row in selected]
        print(f"{phase:12s} {np.mean(currents):.4f} mA  {np.mean(powers):.4f} mW"
              f"  range={min(powers):.4f}..{max(powers):.4f} mW")


if __name__ == "__main__":
    main()
