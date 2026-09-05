#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Capture SWO output from a running board into a timestamped log.

Usage:
    python3 tools/bench/swo_capture.py DURATION OUTPUT [--app-dir DIR]
                                       [--board BOARD]

Runs `uv run nsx view` in its own process group and writes every line it
produces to OUTPUT, prefixed with a host wall-clock timestamp
(`<epoch_seconds>.<ms> <line>`). The host clock is what makes the log
analysable: `hkv_analyze.py` compares it against the firmware uptime, and
`hkv_compare.py` turns cumulative counters into rates with it.

Capture must go through `nsx view`. Calling a J-Link binary directly picks
the wrong SWO clock and silently produces an empty log. See #39.

Arguments:
    DURATION    Capture window in seconds.
    OUTPUT      Log file to write. Overwritten if it exists.
    --app-dir   Application directory passed to nsx. Default: current dir.
    --board     NSX board name. Default: apollo510b_evb.

Exit codes:
    0   Capture completed and the log has at least one line.
    1   nsx could not be started, or it started and produced no lines. The
        two are reported differently: a start failure is a host problem
        (`uv` not on PATH), while an empty log is a stop-and-report condition
        at the board, not something to work around with a different tool.
    2   Bad arguments (argparse).
"""

import argparse
import os
import select
import signal
import subprocess
import sys
import time

VIEWER_PROCESS = "JLinkSWOViewerCL"


def kill_stale_viewers():
    subprocess.run(["pkill", "-9", "-f", VIEWER_PROCESS], capture_output=True, check=False)


def capture(duration, output, app_dir, board):
    kill_stale_viewers()
    cmd = ["uv", "run", "nsx", "view", "--app-dir", app_dir, "--board", board]
    try:
        # Own process group: the viewer holds the pipe open, so a plain kill leaks it. See #39.
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            cwd=app_dir,
        )
    except OSError as exc:
        # A host fault, not a board fault: do not send the reader to the bench. See #39.
        print(f"could not start {' '.join(cmd)}: {exc}", file=sys.stderr)
        return None

    started = time.time()
    lines = 0
    with open(output, "w") as log:
        while time.time() - started < duration:
            ready, _, _ = select.select([proc.stdout], [], [], 1.0)
            if not ready:
                continue
            line = proc.stdout.readline()
            if not line:
                break
            log.write(f"{time.time():.3f} {line.decode(errors='replace')}")
            lines += 1

    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        pass
    kill_stale_viewers()
    print(f"captured {lines} lines in {time.time() - started:.0f}s")
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Capture nsx SWO output to a host-timestamped log."
    )
    parser.add_argument("duration", type=float, help="capture window in seconds")
    parser.add_argument("output", help="log file to write")
    parser.add_argument("--app-dir", default=".", help="application directory (default: .)")
    parser.add_argument(
        "--board", default="apollo510b_evb", help="NSX board name (default: apollo510b_evb)"
    )
    args = parser.parse_args(argv)

    if args.duration <= 0:
        parser.error("duration must be greater than zero")
    if not os.path.isdir(args.app_dir):
        parser.error(f"app-dir does not exist: {args.app_dir}")

    lines = capture(args.duration, args.output, args.app_dir, args.board)
    if lines is None:
        return 1
    if lines == 0:
        print(
            f"no SWO lines captured into {args.output}; stop and report rather than "
            "trying another capture tool",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
