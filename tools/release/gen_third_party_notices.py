#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Generate THIRD-PARTY-NOTICES.md from the modules pinned in nsx.lock.

The firmware this repo builds links code from a dozen vendored modules, several
of which carry notice-reproduction obligations that bind BINARY redistribution:
the Ambiq Apollo SDK License (helia-rt, ns-cmsis-nn), Apache-2.0 section 4(b)
(helia-dsp, ns-cmsis-nn upstream), BSD-3-Clause clause 2 (the nsx modules), the
AmbiqSuite software agreement, and the ams-OSRAM agreement covering the AS7058
driver. The release package therefore has to carry the license texts, not just
a list of names.

The module tree under `modules/` is generated from `nsx.lock` and is gitignored,
so the notices file is generated from that tree and committed, letting a clone
without `modules/` still ship the notices.

Usage:

    python3 tools/release/gen_third_party_notices.py            # all lock targets
    python3 tools/release/gen_third_party_notices.py --board apollo510b_evb
    python3 tools/release/gen_third_party_notices.py --check    # fail if stale
    python3 tools/release/gen_third_party_notices.py -o /path/THIRD-PARTY-NOTICES.md

Properties this script is required to keep:

- Deterministic. Everything is sorted; nothing records a timestamp, a hostname
  or a local absolute path. Re-running it on an unchanged tree rewrites the
  same bytes, so `--check` is a meaningful CI gate.
- Repo-relative paths only. The generated file ships to customers.
- Fail closed. A required component whose license file has moved or gone
  missing is an error, not a silent omission.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LOCK = REPO_ROOT / "nsx.lock"
DEFAULT_OUTPUT = REPO_ROOT / "THIRD-PARTY-NOTICES.md"

# ---------------------------------------------------------------------------
# What counts as a notice document
# ---------------------------------------------------------------------------

# Matched on the exact file name, case-insensitively, anywhere under a module.
LICENSE_FILENAMES = {
    "license",
    "license.md",
    "license.txt",
    "licence",
    "licence.txt",
    "copying",
    "copying.txt",
    "notice",
    "notice.md",
    "notice.txt",
    "third_party_notices.md",
    "third-party-notices.md",
    "third-party-payload.md",
}

# The AmbiqSuite payload keeps its upstream license bundle in one directory
# rather than next to each component, so every text in that directory is a
# notice for something in the SDK. Taken wholesale, minus the exclusions below.
LICENSE_BUNDLE_DIRS = ("sdk/docs/licenses",)
LICENSE_BUNDLE_SUFFIXES = {".txt", ".md"}

# filelist.txt is a packaging manifest, not a license.
#
# gpl-3.0.txt is deliberately NOT reproduced. AmbiqSuite's own
# sdk/docs/licenses/filelist.txt annotates it as "Tied to LVGL!", LVGL is not
# referenced anywhere in this app's build (zero hits in build.ninja), no
# GPL-headed source under the SDK payload is compiled, and the linker map
# contains no GPL strings. Nothing GPL-licensed is linked into the shipped
# firmware, so reproducing the GPL text here would misstate what the binary
# contains. If LVGL is ever enabled, delete this exclusion first.
BUNDLE_EXCLUDE = {"filelist.txt", "gpl-3.0.txt"}

# Binary formats that cannot be reproduced inline; referenced by path instead.
REFERENCE_ONLY_SUFFIXES = {".pdf"}

# Directories never worth walking.
SKIP_DIRS = {".git", ".github", "__pycache__", "build", "node_modules"}

# ---------------------------------------------------------------------------
# Components that must be present. A miss is a hard error: these are the
# obligations the 2026-09-02 license inventory (issue #41) found binding on the
# shipped binary, and a silent drop would ship the firmware without them.
# ---------------------------------------------------------------------------
REQUIRED_PATHS = [
    "modules/helia-dsp/LICENSE",
    "modules/helia-dsp/Ne10/LICENSE",
    "modules/helia-dsp/ComputeLibrary/LICENSE.txt",
    "modules/helia-rt/LICENSE",
    "modules/helia-rt/THIRD_PARTY_NOTICES.md",
    "modules/ns-cmsis-nn/LICENSE",
    "modules/ns-cmsis-nn/NOTICE",
    "modules/nsx-as7058/license.txt",
    "modules/nsx-physiokit/LICENSE",
    "modules/nsx-pmu-armv8m/LICENSE",
    "modules/nsx-ambiq-sdk/modules/nsx-core/LICENSE",
    "modules/nsx-ambiq-sdk/modules/nsx-freertos/sdk/third_party/FreeRTOS-Kernel/LICENSE.md",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/LICENSE.rtf",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/SEGGER-RTT-license.txt",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/ThinkSi-license.txt",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/MBEDtls_Apache2.txt",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/OpenAmp_BSD-3.txt",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/LibMetal_BSD.txt",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/zlib.txt",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/jQuery.txt",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/Opus-License-Info.txt",
    "modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/docs/licenses/LICENSE_COREMARK.md",
]

# ---------------------------------------------------------------------------
# Gap fills. Some modules are vendored as source drops with the upstream
# LICENSE file stripped. The upstream repositories are BSD-3-Clause, so the
# text is carried here with the source repo and the exact pin it was taken
# against; the pin comes from nsx.lock at generation time.
# ---------------------------------------------------------------------------
BSD_3_CLAUSE_AMBIQ = """BSD 3-Clause License

Copyright (c) 2026, Ambiq
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."""

# module directory -> (upstream repo, SPDX id, explanation)
MISSING_LICENSE_FILLS = {
    "modules/nsx-sensors": (
        "https://github.com/AmbiqAI/nsx-sensors",
        "BSD-3-Clause",
        BSD_3_CLAUSE_AMBIQ,
    ),
    "modules/nsx-tileio": (
        "https://github.com/AmbiqAI/nsx-tileio",
        "BSD-3-Clause",
        BSD_3_CLAUSE_AMBIQ,
    ),
}


# ---------------------------------------------------------------------------
# nsx.lock
# ---------------------------------------------------------------------------


def load_lock(path: Path) -> dict:
    try:
        import yaml  # type: ignore
    except ModuleNotFoundError:
        sys.exit(
            "error: PyYAML is required. Run this through the project environment:\n"
            "       uv run python tools/release/gen_third_party_notices.py"
        )
    with path.open(encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def module_pins(lock: dict, boards: list[str]) -> dict[str, dict]:
    """Map a vendored module directory to its repo URL and the pins in use.

    Keyed by `vendored_at` because that is the directory the notices are read
    from. One directory can back several logical modules (every nsx-ambiq-sdk
    module resolves to `modules/nsx-ambiq-sdk`), so names accumulate.
    """
    targets = lock.get("targets", {})
    unknown = [b for b in boards if b not in targets]
    if unknown:
        sys.exit(
            f"error: board(s) not in {LOCK.name}: {', '.join(sorted(unknown))}; "
            f"available: {', '.join(sorted(targets))}"
        )

    out: dict[str, dict] = {}
    for board in sorted(boards):
        for name, mod in sorted(targets[board].get("modules", {}).items()):
            if mod.get("kind") != "git":
                # `packaged` modules are this repo's own board definitions and
                # the generated cmake/nsx tooling; both are covered by LICENSE.
                continue
            resolved = mod.get("resolved", {})
            vendored = resolved.get("vendored_at")
            if not vendored:
                continue
            entry = out.setdefault(
                vendored,
                {"url": resolved.get("url", ""), "names": set(), "pins": set(), "boards": set()},
            )
            entry["names"].add(name)
            entry["boards"].add(board)
            pin = resolved.get("tag") or resolved.get("commit") or mod.get("constraint") or ""
            commit = resolved.get("commit") or ""
            if pin and commit and pin != commit:
                entry["pins"].add(f"{pin} ({commit[:12]})")
            elif pin:
                entry["pins"].add(pin[:12] if pin == commit else pin)
    return out


# ---------------------------------------------------------------------------
# Declared SPDX ids from nsx-module.yaml
# ---------------------------------------------------------------------------


def declared_licenses(root: Path) -> dict[Path, tuple[str, Path]]:
    """Directory -> (declared license id, manifest that declared it).

    Only a handful of modules declare `license.type`; the rest are reported as
    "not declared" and the license text itself is the source of truth.
    """
    import yaml  # imported lazily by load_lock() first

    out: dict[Path, tuple[str, Path]] = {}
    for manifest in sorted(root.rglob("nsx-module.yaml")):
        if set(manifest.parts) & SKIP_DIRS:
            continue
        try:
            with manifest.open(encoding="utf-8") as fh:
                data = yaml.safe_load(fh) or {}
        except Exception:
            continue
        lic = data.get("license")
        if not isinstance(lic, dict):
            continue
        ident = lic.get("type") or lic.get("spdx")
        if not ident:
            continue
        if ident == "proprietary" and lic.get("owner"):
            ident = f"proprietary ({lic['owner']})"
        # A manifest under `nsx/` describes the module one level up.
        scope = manifest.parent
        if scope.name in {"nsx", "cmake"}:
            scope = scope.parent
        out[scope] = (str(ident), manifest)
    return out


def declared_for(path: Path, table: dict[Path, tuple[str, Path]]) -> tuple[str, str] | None:
    """Nearest enclosing declaration for a license file."""
    for parent in [path, *path.parents]:
        if parent in table:
            ident, manifest = table[parent]
            return ident, rel(manifest)
    return None


# ---------------------------------------------------------------------------
# Collecting notice documents
# ---------------------------------------------------------------------------


def rel(path: Path) -> str:
    return path.relative_to(REPO_ROOT).as_posix()


def is_bundle_file(path: Path) -> bool:
    posix = path.as_posix()
    return (
        any(f"/{d}/" in posix for d in LICENSE_BUNDLE_DIRS)
        and path.suffix.lower() in (LICENSE_BUNDLE_SUFFIXES | REFERENCE_ONLY_SUFFIXES)
        and path.name.lower() not in BUNDLE_EXCLUDE
    )


def collect(module_dir: Path) -> list[Path]:
    found: list[Path] = []
    for path in module_dir.rglob("*"):
        if set(path.parts) & SKIP_DIRS:
            continue
        if not path.is_file():
            continue
        name = path.name.lower()
        if name in LICENSE_FILENAMES or name == "license.rtf" or is_bundle_file(path):
            found.append(path)
    return sorted(found, key=lambda p: (p.parent.as_posix(), p.name))


def read_text(path: Path) -> str:
    """License text, verbatim apart from line-ending and trailing-space cleanup.

    RTF is converted to plain text with macOS `textutil` when it is available;
    without it the reader is pointed at the file and at the PDF beside it,
    because a mangled license text is worse than a pointer to the real one.
    """
    if path.suffix.lower() == ".rtf":
        textutil = shutil.which("textutil")
        if not textutil:
            return (
                f"[Not reproduced inline: `{rel(path)}` is RTF and no converter "
                "(macOS `textutil`) was available when this file was generated. "
                "The same agreement is provided as "
                "`sdk/docs/licenses/Ambiq-Software-License-Terms.pdf` in the "
                "AmbiqSuite payload; both files ship inside the module tree.]"
            )
        proc = subprocess.run(
            [textutil, "-convert", "txt", "-stdout", str(path)],
            capture_output=True,
            check=False,
        )
        if proc.returncode != 0:
            return (
                f"[Not reproduced inline: converting `{rel(path)}` with textutil "
                f"failed (exit {proc.returncode}). See "
                "`sdk/docs/licenses/Ambiq-Software-License-Terms.pdf`.]"
            )
        raw = proc.stdout.decode("utf-8", errors="replace")
    elif path.suffix.lower() in REFERENCE_ONLY_SUFFIXES:
        return f"[Not reproduced inline: `{rel(path)}` is a binary PDF. See that file.]"
    else:
        raw = path.read_bytes().decode("utf-8", errors="replace")

    raw = raw.replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.rstrip() for line in raw.split("\n")]
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines)


def fence_for(text: str) -> str:
    """A fence longer than the longest backtick run in the text."""
    longest = max((len(m) for m in re.findall(r"`+", text)), default=0)
    return "`" * max(3, longest + 1)


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------


def render(boards: list[str], modules: dict[str, dict]) -> str:
    root_marker = REPO_ROOT

    out: list[str] = []
    w = out.append

    w("# Third-party notices")
    w("")
    w(
        "Generated by `tools/release/gen_third_party_notices.py` from the modules "
        "pinned in `nsx.lock`. Do not edit by hand."
    )
    w("")
    w(
        "The HeartKit Vitals demo firmware is built from Ambiq-authored source in "
        "this repository, licensed under the BSD 3-Clause License (see `LICENSE`), "
        "linked against the third-party components listed below. Each component is "
        "reproduced with the license text as it appears in the vendored module "
        "tree. Paths are relative to the repository root; the `modules/` tree "
        "itself is generated from `nsx.lock` and is not committed."
    )
    w("")
    w("Boards covered: " + ", ".join(f"`{b}`" for b in sorted(boards)) + ".")
    w("")
    w(
        "The AS7058 sensor driver (`modules/nsx-as7058`) is proprietary ams-OSRAM "
        "software supplied to Ambiq under agreement. It is distributed only in "
        "binary form as part of the prebuilt firmware; its source is not in this "
        "repository and building from source requires access to the private "
        "`nsx-as7058` module."
    )
    w("")
    w(
        "LVGL is not built into this firmware, so the GPL-3.0 text carried in the "
        "AmbiqSuite license bundle (`sdk/docs/licenses/gpl-3.0.txt`, annotated "
        '"Tied to LVGL" in that bundle\'s `filelist.txt`) is deliberately not '
        "reproduced here: no GPL-licensed component is linked into the shipped "
        "image."
    )
    w("")

    # ---- module summary -------------------------------------------------
    w("## Modules covered")
    w("")
    w("| Module directory | Upstream | Pin |")
    w("| --- | --- | --- |")
    for vendored in sorted(modules):
        info = modules[vendored]
        url = info["url"] or "n/a"
        pins = ", ".join(f"`{p}`" for p in sorted(info["pins"])) or "n/a"
        w(f"| `{vendored}` | {url} | {pins} |")
    w("")

    # ---- components -----------------------------------------------------
    w("## Components")
    w("")

    declared: dict[Path, tuple[str, Path]] = {}
    entries: list[tuple[str, str, list[Path]]] = []
    for vendored in sorted(modules):
        module_dir = root_marker / vendored
        if not module_dir.is_dir():
            sys.exit(
                f"error: module directory {vendored} is missing. Run\n"
                "       uv run nsx configure --app-dir . --board <board>\n"
                "       to materialise modules/ before generating notices."
            )
        declared.update(declared_licenses(module_dir))
        entries.append((vendored, modules[vendored]["url"], collect(module_dir)))

    # Fail closed on the components the license inventory says must be here.
    present = {rel(p) for _, _, files in entries for p in files}
    missing = [p for p in REQUIRED_PATHS if p not in present]
    if missing:
        sys.exit(
            "error: required license files not found:\n  "
            + "\n  ".join(missing)
            + "\nThe module tree moved. Fix REQUIRED_PATHS or re-vendor before shipping."
        )

    # Group by the directory the license file sits in: that directory is the
    # component, whether it is a module root or a vendored subproject.
    groups: dict[str, list[Path]] = {}
    owner: dict[str, tuple[str, str]] = {}
    for vendored, url, files in entries:
        for path in files:
            key = rel(path.parent)
            groups.setdefault(key, []).append(path)
            owner.setdefault(key, (vendored, url))

    for key in sorted(groups):
        vendored, url = owner[key]
        w(f"### `{key}`")
        w("")
        info = modules[vendored]
        pins = ", ".join(f"`{p}`" for p in sorted(info["pins"])) or "n/a"
        w(f"- Component path: `{key}`")
        w(f"- Vendored from: {url or 'n/a'} at {pins} (`{vendored}`)")
        decl = declared_for(root_marker / key, declared)
        if decl:
            w(f"- Declared license: `{decl[0]}` (declared in `{decl[1]}`)")
        else:
            w("- Declared license: not declared in an `nsx-module.yaml`; see the text below.")
        w("")
        for path in sorted(groups[key], key=lambda p: p.name):
            text = read_text(path)
            w(f"#### `{rel(path)}`")
            w("")
            fence = fence_for(text)
            w(fence)
            w(text)
            w(fence)
            w("")

    # ---- gap fills ------------------------------------------------------
    fills = sorted(k for k in MISSING_LICENSE_FILLS if k in modules)
    if fills:
        w("## Components vendored without a license file")
        w("")
        w(
            "These modules are vendored as source drops that do not carry the "
            "upstream `LICENSE` file. The upstream repositories are BSD-3-Clause; "
            "the text is reproduced here against the pin actually built."
        )
        w("")
        for vendored in fills:
            upstream, spdx, text = MISSING_LICENSE_FILLS[vendored]
            info = modules[vendored]
            pins = ", ".join(f"`{p}`" for p in sorted(info["pins"])) or "n/a"
            w(f"### `{vendored}`")
            w("")
            w(f"- Component path: `{vendored}`")
            w(f"- Vendored from: {info['url'] or upstream} at {pins}")
            w(f"- License: `{spdx}`, text taken from the upstream repository {upstream}")
            w(
                "- Note: the vendored drop contains no `LICENSE` file; this is the "
                "upstream license for the pin above, not a file read from the tree."
            )
            w("")
            fence = fence_for(text)
            w(fence)
            w(text)
            w(fence)
            w("")

    while out and out[-1] == "":
        out.pop()
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--board",
        action="append",
        default=None,
        help="Lock target to cover; repeatable. Default: every target in nsx.lock.",
    )
    ap.add_argument(
        "-o",
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help="Destination file (default: THIRD-PARTY-NOTICES.md at the repo root).",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="Do not write; exit 1 if the destination differs from what would be generated.",
    )
    args = ap.parse_args(argv)

    if not LOCK.is_file():
        sys.exit(f"error: {rel(LOCK)} not found")

    lock = load_lock(LOCK)
    boards = args.board or sorted(lock.get("targets", {}))
    if not boards:
        sys.exit(f"error: no targets in {rel(LOCK)}")

    modules = module_pins(lock, boards)
    text = render(boards, modules)

    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != text:
            print(
                f"error: {args.output.name} is out of date; "
                "re-run tools/release/gen_third_party_notices.py",
                file=sys.stderr,
            )
            return 1
        print(f"{args.output.name} is up to date")
        return 0

    args.output.write_text(text, encoding="utf-8")
    try:
        shown = rel(args.output)
    except ValueError:
        shown = args.output.name
    print(f"wrote {shown} ({len(text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
