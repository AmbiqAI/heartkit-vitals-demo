#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Run one gate capture with the Python USB host instead of the browser.

Usage:
    python3 tools/bench/usb_bench.py --speed lp --seconds 180 --log /tmp/bench.log
                                     [--settle S] [--app-dir DIR] [--board BOARD]
                                     [--dry-run] [-- EXTRA_SWO_ARGS]

Starts tools/tileio_usb_test.py so the IN endpoint is drained for the whole
run (an undrained endpoint is what makes the firmware transport counters
move), sets the speed mode over UIO, waits for the figures to settle, then
captures SWO with tools/bench/swo_capture.py. The USB host keeps reading
until the capture is over. See #37.

The board must already be flashed: this tool never programs anything.

Exit codes:
    0   Capture finished, log written, and the host drained packets without CRC
        errors for the whole window.
    1   USB host or SWO capture failed, or the host reported CRC errors, no
        packets, or a run shorter than the capture window.
    2   Bad arguments (argparse).
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
USB_TEST = os.path.join(REPO_ROOT, "tools", "tileio_usb_test.py")
SWO_CAPTURE = os.path.join(REPO_ROOT, "tools", "bench", "swo_capture.py")

# The host outlives the SWO capture by this margin so nothing stops draining
# while swo_capture.py is still writing lines.
HOLD_S = 5.0

# The host's own clock and the capture's start never line up exactly; anything
# beyond this is a host that stopped draining early.
DURATION_SLACK_S = 1.0

# Seconds to let a terminated host flush its summary line before killing it.
TERM_GRACE_S = 5.0

SUMMARY_RE = re.compile(
    r"^summary: packets=(\d+) bytes=(\d+) crc_errors=(\d+) duration=([\d.]+)", re.M)


def build_commands(args, swo_extra):
    host_duration = args.settle + args.seconds + HOLD_S
    usb_cmd = [sys.executable, USB_TEST, "--duration", f"{host_duration:g}"]
    if args.speed:
        usb_cmd += ["--speed", args.speed]
    swo_cmd = [sys.executable, SWO_CAPTURE, f"{args.seconds:g}", args.log,
               "--app-dir", args.app_dir, "--board", args.board] + swo_extra
    return usb_cmd, swo_cmd


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Drive a bench capture from the Python USB host.",
        epilog="Arguments after `--` are passed through to swo_capture.py.",
    )
    parser.add_argument("--speed", choices=("lp", "hp"), default=None,
                        help="speed mode to request over UIO (default: leave the stored mode)")
    parser.add_argument("--seconds", type=float, default=180.0,
                        help="SWO capture window in seconds (default: 180)")
    parser.add_argument("--settle", type=float, default=10.0,
                        help="seconds to stream before capturing (default: 10)")
    parser.add_argument("--log", required=True, help="SWO log file to write")
    parser.add_argument("--app-dir", default=".", help="application directory (default: .)")
    parser.add_argument("--board", default="apollo510b_evb",
                        help="NSX board name (default: apollo510b_evb)")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the two subprocess commands and exit")
    args, swo_extra = parser.parse_known_args(argv)
    if swo_extra and swo_extra[0] == "--":
        swo_extra = swo_extra[1:]

    if args.seconds <= 0:
        parser.error("--seconds must be greater than zero")
    if args.settle < 0:
        parser.error("--settle must not be negative")

    usb_cmd, swo_cmd = build_commands(args, swo_extra)

    if args.dry_run:
        print("usb host: " + shlex.join(usb_cmd))
        print(f"settle:   {args.settle:g}s")
        print("swo:      " + shlex.join(swo_cmd))
        return 0

    if not os.path.isdir(args.app_dir):
        parser.error(f"app-dir does not exist: {args.app_dir}")

    usb_log = args.log + ".usb"
    print("starting USB host: " + shlex.join(usb_cmd))
    with open(usb_log, "w") as usb_out:
        try:
            usb_proc = subprocess.Popen(usb_cmd, stdout=usb_out, stderr=subprocess.STDOUT)
        except OSError as exc:
            print(f"could not start the USB host: {exc}", file=sys.stderr)
            return 1

        captured = False
        try:
            deadline = time.monotonic() + args.settle
            while time.monotonic() < deadline:
                if usb_proc.poll() is not None:
                    print(f"USB host exited during settle (rc={usb_proc.returncode}); "
                          f"see {usb_log}", file=sys.stderr)
                    return 1
                time.sleep(0.5)

            print("capturing SWO: " + shlex.join(swo_cmd))
            swo_rc = subprocess.call(swo_cmd)
            captured = swo_rc == 0
        except KeyboardInterrupt:
            print("interrupted; stopping the USB host", file=sys.stderr)
            return 1
        finally:
            # Only a clean capture is worth waiting out the host's remaining
            # hold; anything else would block until its --duration expires.
            if not captured:
                usb_proc.terminate()
                try:
                    usb_proc.wait(timeout=TERM_GRACE_S)
                except subprocess.TimeoutExpired:
                    usb_proc.kill()
            usb_rc = usb_proc.wait()

    print(f"USB host output: {usb_log}")
    failed = False
    if usb_rc != 0:
        print(f"USB host failed (rc={usb_rc})", file=sys.stderr)
        failed = True
    if swo_rc != 0:
        print(f"SWO capture failed (rc={swo_rc})", file=sys.stderr)
        failed = True

    with open(usb_log, errors="replace") as fh:
        match = SUMMARY_RE.search(fh.read())
    if match is None:
        print("USB host wrote no summary line", file=sys.stderr)
        failed = True
    else:
        print(match.group(0))
        packets = int(match.group(1))
        crc_errors = int(match.group(3))
        duration = float(match.group(4))
        if packets == 0:
            print("USB host drained no packets; the SWO figures are not a "
                  "streaming workload", file=sys.stderr)
            failed = True
        if crc_errors != 0:
            print(f"USB host reported {crc_errors} CRC errors", file=sys.stderr)
            failed = True
        min_duration = args.settle + args.seconds - DURATION_SLACK_S
        if duration < min_duration:
            print(f"USB host ran {duration:g}s, short of the {min_duration:g}s the "
                  f"capture window needed", file=sys.stderr)
            failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(1)
