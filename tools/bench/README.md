<!--
SPDX-License-Identifier: BSD-3-Clause
Copyright (c) 2026, Ambiq
-->

# Bench tools

Host-side tools for capturing and reading firmware telemetry over SWO.

| Script | Purpose |
| --- | --- |
| `swo_capture.py` | Capture `nsx view` output to a host-timestamped log |
| `hkv_analyze.py` | Settled means, timebase ratio and packet rates from one capture |
| `hkv_compare.py` | Transport counter deltas and quality distributions across captures |

Each script takes `--help`, exits 0 on success, 1 on a data or capture
failure, and 2 on bad arguments.

## The one rule

Captures go through `nsx`. `swo_capture.py` runs `uv run nsx view` and
nothing else. Do not reach for `JLinkSWOViewerCL` or any other J-Link binary
directly: it picks the wrong SWO clock and produces a log that is empty or
silently truncated, which reads as a passing run with no telemetry.

If a capture comes back empty, stop and report it. An empty log is a fault in
the board, the debugger connection or the build, and it is never fixed by
trying a different capture tool.

## Typical use

```bash
python3 tools/bench/swo_capture.py 180 /tmp/bench-run.log --app-dir . --board apollo510b_evb
python3 tools/bench/hkv_analyze.py /tmp/bench-run.log --label run
python3 tools/bench/hkv_compare.py /tmp/bench-baseline.log /tmp/bench-run.log
```

`hkv_compare.py` reports cumulative firmware counters as deltas and rates.
The counters never reset while the board runs, so a raw total carries no
information about the window you captured. The one exception is `ecg_ok`,
whose first and last values are printed and labelled `cumulative`, to show
which window the deltas next to them were taken over.
