#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Flash and measure one AP510B sleep-memory case with JS110 and a clean reset."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

import numpy as np
from pyjls import Reader
from lp_power_report import read_interval

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/apollo510b_evb"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tcm", type=int, choices=(160, 384, 768), required=True)
    p.add_argument("--sram", type=int, choices=range(4), required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--hpx-python", type=Path, required=True,
                   help="Python executable in the heliaPROFILER environment")
    p.add_argument("--probe-serial", required=True)
    p.add_argument("--joulescope-serial", required=True)
    args = p.parse_args()
    if not re.fullmatch(r"[a-z0-9-]+", args.name):
        p.error("name must contain lowercase letters, digits or hyphens")
    hpx_python = args.hpx_python.expanduser().resolve()
    if not hpx_python.is_file():
        p.error("hpx-python must be an existing Python executable")
    hpx = args.hpx_python.expanduser().absolute().parent / "hpx"
    if not hpx.is_file():
        p.error("hpx must be installed beside hpx-python")
    out = ROOT / "tools/bench/results/lp-power-20260909/sleep-bank-sweep" / args.name
    out.mkdir(parents=True, exist_ok=False)

    def command(label, *cmd, **kwargs):
        result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                                timeout=90, **kwargs)
        (out / (label + ".log")).write_text(result.stdout + result.stderr)
        result.check_returncode()
        return result.stdout

    print(f"Preparing {args.name}: TCM={args.tcm} KiB, SRAM={args.sram} MiB", flush=True)
    command("configure", "cmake", "-S", str(ROOT), "-B", str(BUILD),
            "-DHKV_BUILD_LP_POWER_TEST=ON", "-DHKV_SLEEP_SINGLE_MRAM=ON",
            "-DHKV_SLEEP_MRAM_LOW_POWER_READ=ON", f"-DHKV_SLEEP_TCM_KIB={args.tcm}",
            f"-DHKV_SLEEP_SRAM_MIB={args.sram}")
    command("build", "cmake", "--build", str(BUILD), "--target", "hkv_sleep_minimal", "-j", "6")
    command("layout", sys.executable, "tools/bench/check_sleep_minimal.py", str(BUILD))
    command("flash", str(ROOT / ".venv/bin/nsx"), "flash", "--board", "apollo510b_evb",
            "--target", "hkv_sleep_minimal", "--probe-serial", args.probe_serial, "--frozen")
    digest = hashlib.sha256((BUILD / "hkv_sleep_minimal.bin").read_bytes()).hexdigest()
    with (out / "record.log").open("w") as log:
        rec = subprocess.Popen([sys.executable, "-u", "-m", "pyjoulescope_driver", "record",
            "--serial_number", args.joulescope_serial, "--open", "restore", "--duration", "60",
            "--frequency", "100000", "--set", "s/i/lsb_src=gpi0", "--set",
            "s/extio/voltage=1.8V", "--signals", "i,v,p,0", str(out / "capture.jls")],
            cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        try:
            time.sleep(1)
            if rec.poll() is not None:
                raise RuntimeError("Joulescope recorder exited before reset")
            command("reset", str(hpx), "target", "reset", "--board",
                    "apollo510b_evb", "--jlink-serial", args.probe_serial, "--kind", "swpoi")
            print(f"Recording {args.name}; no debug attachment during capture", flush=True)
            if rec.wait(timeout=70):
                raise RuntimeError("Joulescope recorder failed")
        finally:
            if rec.poll() is None:
                rec.send_signal(signal.SIGINT)
                rec.wait(timeout=10)

    with Reader(str(out / "capture.jls")) as reader:
        signals = {s.name: s for s in reader.signals.values()}
        gate = signals["gpi[0]"]
        bits = np.unpackbits(reader.fsr(gate.signal_id, 0, gate.length), bitorder="little")[:gate.length]
        assert len(bits) >= 5500000 and np.all(bits[3000000:5500000]), "Gate not high in settled interval"
        edges = np.flatnonzero(np.diff(bits.astype(np.int8))) + 1
        assert any(20 < i / gate.sample_rate < 30 and bits[i] for i in edges), "No startup sleep edge"
        start = reader.sample_id_to_timestamp(gate.signal_id, 3000000)
        end = reader.sample_id_to_timestamp(gate.signal_id, 5500000)
        values = {name: float(read_interval(reader, signals[name], start, end).mean())
                  for name in ("current", "voltage", "power")}
        blocks = []
        for sec in range(30, 55, 5):
            a = reader.sample_id_to_timestamp(gate.signal_id, sec * gate.sample_rate)
            b = reader.sample_id_to_timestamp(gate.signal_id, (sec + 5) * gate.sample_rate)
            blocks.append(float(read_interval(reader, signals["power"], a, b).mean()))

    configs = list((BUILD / "CMakeFiles").glob("*/CMakeCCompiler.cmake"))
    assert len(configs) == 1
    compiler = re.search(r'set\(CMAKE_C_COMPILER "([^"]+)"\)', configs[0].read_text())[1]
    nm = command("symbols", compiler.removesuffix("gcc") + "nm", str(BUILD / "hkv_sleep_minimal"))
    def address(name):
        return int(re.search(rf"^([0-9a-f]+) \w {name}$", nm, re.M)[1], 16)
    code = f'''import json
from helia_profiler.target.probe.jlink import attached_session
with attached_session(device="AP510NFA-CBR", jlink_serial={args.probe_serial!r}, attach_timeout_s=10) as j:
 print(json.dumps(dict(snapshot=list(j.memory_read32({address('g_sleep_snapshot')},18)),
                      wake=j.memory_read32({address('g_sleep_wake_status')},1)[0],
                      flags=j.memory_read32(0x40021020,1)[0])))
'''
    state = json.loads(command("state", str(args.hpx_python.expanduser().absolute()), "-c", code).strip().splitlines()[-1])
    s = state["snapshot"]
    assert s[0] == 2 and s[3] == s[4] == 0, state
    assert s[6] & 7 == {160: 1, 384: 3, 768: 7}[args.tcm], state
    assert s[8] == s[9] == (1 << args.sram) - 1, state
    assert s[12] == 0x205 and s[16] == s[17] == 0, state
    assert state["wake"] == 0 and state["flags"] & 0x20000000, state
    result = dict(tcm_KiB=args.tcm, sram_MiB=args.sram, sha256=digest,
                  start_s=30, end_s=55, values=values, block_power_W=blocks, state=state)
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"PASS {args.name}: {values['current']*1000:.6f} mA, "
          f"{values['power']*1000:.6f} mW; BLE ENABLE latch/pad=0/0", flush=True)


if __name__ == "__main__":
    main()
