#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Render an on-device parity capture as a markdown table.

    python3 tools/aot/parity_report.py parity-run1.log

Reads a log written by tools/bench/swo_capture.py (each line may carry a host
timestamp prefix) and looks for the `HKV|parity|...` records emitted by
tools/aot/parity/main.c.

Exits 1 if any case failed or if `PARITY_DONE` is missing: a capture that was
cut short must not read as a pass. See AmbiqAI/heartkit-vitals-demo#37.
"""

import argparse
import re
import sys

BOOT_RE = re.compile(r"HKV\|parity\|boot\s+(.*)$")
CASE_RE = re.compile(r"HKV\|parity\|(seg|arr)\s+(case=.*)$")
SUMMARY_RE = re.compile(r"HKV\|parity\|summary\s+(.*)$")
SELFCHECK_RE = re.compile(r"HKV\|parity\|selfcheck\s+(.*)$")

COLUMNS = ["case", "max_lsb", "max_abs", "argmax_pct", "valid_pct", "mask_eq", "cycles", "pass"]


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
        record = {"boot": boot, "cases": {"seg": [], "arr": []}, "selfchecks": [], "summary": None,
                  "done": False}
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
                current["summary"] = kv(m.group(1))
                continue
            m = SELFCHECK_RE.search(line)
            if m:
                current["selfchecks"].append(kv(m.group(1)))
                continue
            m = CASE_RE.search(line)
            if m:
                current["cases"][m.group(1)].append(kv(m.group(2)))

    complete = [p for p in passes if p["done"]]
    run = complete[-1] if complete else passes[-1]
    cases, selfchecks = run["cases"], run["selfchecks"]
    summary, boot, done = run["summary"], run["boot"], run["done"]

    failures = []
    for model in ("seg", "arr"):
        rows = cases[model]
        print(f"### {model}\n")
        if not rows:
            print("_no cases captured_\n")
            failures.append(f"{model}: no cases in capture")
            continue
        print("| " + " | ".join(COLUMNS) + " |")
        print("|" + "|".join(["---"] * len(COLUMNS)) + "|")
        for row in rows:
            print("| " + " | ".join(row.get(c, "-") for c in COLUMNS) + " |")
            if row.get("pass") != "1":
                failures.append(f"{model} case {row.get('case', '?')} failed")
        print()

    for sc in selfchecks:
        print(f"- selfcheck {sc.get('model', '?')}: rc={sc.get('rc', '?')}")
        if sc.get("rc") != "0":
            failures.append(f"selfcheck {sc.get('model', '?')} rc={sc.get('rc')}")
    if selfchecks:
        print()

    if boot:
        print("**boot**: " + " ".join(f"{k}={v}" for k, v in boot.items()))
    if summary:
        print("**summary**: " + " ".join(f"{k}={v}" for k, v in summary.items()))
    else:
        failures.append("no summary line in capture")

    if not done:
        failures.append("PARITY_DONE missing")

    if failures:
        print("\nFAIL:", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        return 1
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
