#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Validate a built GCC sleep image and exercise its linker memory limits."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def run(*args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True, **kwargs)


def check(build):
    configs = list((build / "CMakeFiles").glob("*/CMakeCCompiler.cmake"))
    assert len(configs) == 1, "Expected one configured CMake compiler"
    compiler = re.search(r'set\(CMAKE_C_COMPILER "([^"]+)"\)', configs[0].read_text())[1]
    prefix = compiler.removesuffix("gcc")
    elf = build / "hkv_sleep_minimal"
    sections = run(prefix + "objdump", "-h", str(elf), check=True).stdout
    sizes = {}
    for entry in re.finditer(
        r"^\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+"
        r"([0-9a-f]+)[^\n]*\n([^\n]+)", sections, re.M
    ):
        name, size, address, load, flags = entry.groups()
        size, address, load = int(size, 16), int(address, 16), int(load, 16)
        if "ALLOC" not in flags or size == 0:
            continue
        valid = any(lo <= address and address + size <= hi for lo, hi in (
            (0, 32768), (0x20000000, 0x20020000), (0x00410000, 0x00600000)
        ))
        assert valid, f"{name} outside powered memory: {address:#x}+{size}"
        if "LOAD" in flags:
            assert 0x00410000 <= load and load + size <= 0x00600000, name
        sizes[name] = sizes.get(name, 0) + size
    assert sizes[".stack"] > 0 and sizes[".bss"] > 0
    nm = run(prefix + "nm", str(elf), check=True).stdout
    assert re.search(r" T am_hal_PRE_SLEEP_PROCESSING$", nm, re.M)
    for forbidden in ("ucHeap", "tud_task", "vTaskStartScheduler", "hkv_denoise_arena_sram_buffer"):
        assert not re.search(rf"\b{forbidden}$", nm, re.M), forbidden
    disassembly = run(prefix + "objdump", "-d", "--disassemble=am_hal_sysctrl_sleep",
                      str(elf), check=True).stdout
    assert re.search(r"bl\s+[^\n]+<am_hal_PRE_SLEEP_PROCESSING>\n[^\n]+\bwfi\b", disassembly)
    print("PASS: ELF placement, strong pre-WFI hook, no application runtime")

    ninja = (build / "build.ninja").read_text()
    for target in ("heartkit-vitals-demo", "hkv_lp_power", "hkv_sleep_baseline"):
        flags = re.search(rf"^  LINK_FLAGS = [^\n]*{re.escape(target)}\.map[^\n]*$", ninja, re.M)[0]
        assert "hkv_sleep_minimal.ld" not in flags, target
    print("PASS: production and reference targets keep SDK linker layout")

    with tempfile.TemporaryDirectory(prefix="hkv-sleep-layout-") as tmp:
        obj, output = Path(tmp) / "probe.o", Path(tmp) / "probe.elf"
        for section, size, must_pass in ((".bss", 16, True), (".bss", 131073, False),
                                         (".itcm_text", 32769, False), (".shared", 4, False),
                                         (".rodata", 2031617, False)):
            source = (f'char probe[{size}] __attribute__((section("{section}")));\n'
                      'void Reset_Handler(void) {}\n')
            run(compiler, "-mcpu=cortex-m55", "-mthumb", "-x", "c", "-c", "-",
                "-o", str(obj), input=source, check=True)
            result = run(prefix + "ld", "-T", str(build / "hkv_sleep_minimal.ld"),
                         str(obj), "-o", str(output))
            assert (result.returncode == 0) == must_pass, result.stderr
            if not must_pass:
                assert any(message in result.stderr for message in (
                    "overflow", "will not fit", "cannot move location counter backwards"
                )), result.stderr
            print(f"PASS: linker {section} {size} bytes {'accepted' if must_pass else 'rejected'}")
    resident = sum(sizes.get(n, 0) for n in (".stack", ".data", ".bss"))
    print(f"Static DTCM incl stack: {resident} bytes ({resident / 1024:.3f} KiB)")
    print(f"Reserved allocator headroom, not live usage: {sizes['.heap']} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    check(parser.parse_args().build.resolve())
