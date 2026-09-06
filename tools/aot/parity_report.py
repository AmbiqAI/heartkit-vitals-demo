#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Render an on-device parity capture as a markdown table.

    python3 tools/aot/parity_report.py parity-run1.log

Reads a log written by tools/bench/swo_capture.py (each line may carry a host
timestamp prefix) and looks for the `HKV|parity|...` records emitted by
tools/aot/parity/main.c.

Cases carry a `ref=` tag and are reported as one table per reference. Only the
`ref=tflm` table is a gate -- it holds the AOT output against the same
flatbuffer run through TFLM on the same device, so a mismatch is attributable
to the AOT compiler. The `ref=golden` table is the host LiteRT capture and is
informational: it also carries LiteRT-vs-TFLM kernel differences.

Exits 1 if a tflm case failed, if the tflm table is absent, or if `PARITY_DONE`
is missing: a capture that was cut short must not read as a pass.
See AmbiqAI/heartkit-vitals-demo#37.
"""

import argparse
import re
import sys

BOOT_RE = re.compile(r"HKV\|parity\|boot\s+(.*)$")
CASE_RE = re.compile(r"HKV\|parity\|(seg|arr)\s+((?:mode|case)=.*)$")
SUMMARY_RE = re.compile(r"HKV\|parity\|summary\s+(.*)$")
SELFCHECK_RE = re.compile(r"HKV\|parity\|selfcheck\s+(.*)$")
CYCLES_RE = re.compile(r"HKV\|parity\|cycles\s+(.*)$")

COLUMNS = ["mode", "case", "max_lsb", "max_abs", "argmax_pct", "valid_pct", "mask_eq", "cycles", "ref_cycles",
           "pass"]

# The only reference whose failures gate. See the module docstring.
GATE_REF = "tflm"
REFS = (GATE_REF, "golden")


def kv(text):
    out = {}
    for token in text.split():
        if "=" in token:
            key, _, value = token.partition("=")
            out[key] = value
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", help="capture log to parse")
    args = ap.parse_args()

    # One capture can hold several passes: attaching the SWO viewer resets this
    # secure-reset SoC, and a manual reset starts another. Report the last pass
    # that reached PARITY_DONE, falling back to the last one seen.
    passes = []

    def new_pass(boot):
        record = {"boot": boot, "cases": {"seg": [], "arr": []}, "selfchecks": [], "summaries": [],
                  "cycles": [], "done": False}
        passes.append(record)
        return record

    current = new_pass(None)

    with open(args.log, "r", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            m = BOOT_RE.search(line)
            if m:
                current = new_pass(kv(m.group(1)))
                continue
            if "PARITY_DONE" in line:
                current["done"] = True
                continue
            m = SUMMARY_RE.search(line)
            if m:
                current["summaries"].append(kv(m.group(1)))
                continue
            m = SELFCHECK_RE.search(line)
            if m:
                current["selfchecks"].append(kv(m.group(1)))
                continue
            m = CYCLES_RE.search(line)
            if m:
                current["cycles"].append(kv(m.group(1)))
                continue
            m = CASE_RE.search(line)
            if m:
                current["cases"][m.group(1)].append(kv(m.group(2)))

    complete = [p for p in passes if p["done"]]
    run = complete[-1] if complete else passes[-1]
    cases, selfchecks = run["cases"], run["selfchecks"]
    summaries, boot, done = run["summaries"], run["boot"], run["done"]

    failures = []
    notes = []

    for ref in REFS:
        gate = ref == GATE_REF
        sink = failures if gate else notes
        rows_by_model = {m: [r for r in cases[m] if r.get("ref") == ref] for m in ("seg", "arr")}
        if not any(rows_by_model.values()):
            if gate:
                failures.append("no ref=tflm cases in capture (was HKV_PARITY_TFLM built?)")
            continue

        print(f"## ref={ref} ({'gate' if gate else 'informational'})\n")
        for model in ("seg", "arr"):
            rows = rows_by_model[model]
            print(f"### {model}\n")
            if not rows:
                print("_no cases captured_\n")
                sink.append(f"{ref} {model}: no cases in capture")
                continue
            print("| " + " | ".join(COLUMNS) + " |")
            print("|" + "|".join(["---"] * len(COLUMNS)) + "|")
            for row in rows:
                print("| " + " | ".join(row.get(c, "-") for c in COLUMNS) + " |")
                if row.get("pass") != "1":
                    sink.append(f"{ref} {model} {row.get('mode', '?')} case {row.get('case', '?')} failed")
            print()

        for summary in (s for s in summaries if s.get("ref") == ref):
            print("**summary**: " + " ".join(f"{k}={v}" for k, v in summary.items()))
        print()

    if run["cycles"]:
        print("### cycles per run (same image, same operating point)\n")
        print("| mode | model | aot | tflm | clk_hz |")
        print("|---|---|---|---|---|")
        for row in run["cycles"]:
            print("| " + " | ".join(row.get(c, "-") for c in ("mode", "model", "aot", "tflm", "clk_hz")) + " |")
        print()

    # A TFLM AllocateTensors() failure would otherwise show up only as eight
    # identical failing cases, so it gates directly; the AOT self-checks are the
    # generator's own vectors and stay informational alongside ref=golden.
    for sc in selfchecks:
        model = sc.get("model", "?")
        print(f"- selfcheck {model}: rc={sc.get('rc', '?')}")
        if sc.get("rc") != "0":
            sink = failures if model.startswith("tflm") else notes
            sink.append(f"selfcheck {model} rc={sc.get('rc')}")
    if selfchecks:
        print()

    if boot:
        print("**boot**: " + " ".join(f"{k}={v}" for k, v in boot.items()))
    if not summaries:
        failures.append("no summary line in capture")

    if not done:
        failures.append("PARITY_DONE missing")

    if notes:
        print("\nInformational (not gated):", file=sys.stderr)
        for n in notes:
            print(f"  - {n}", file=sys.stderr)
    if failures:
        print("\nFAIL:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
