#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Record and check the provenance of the vendored dependency modules.

The app repo's own dirty flag says nothing about `modules/`: that tree is
ignored via `modules/.gitignore`, so a locally patched dependency would ship
silently. This script records what is actually on disk and refuses to package
when it can prove a module diverges from `nsx.lock`.

Exit codes:
  0  provenance recorded, safe to package (warnings may still be present)
  1  refuse to package
  2  usage or parse error

The report is written to stdout for inclusion in BUILD-INFO.txt.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    print("error: PyYAML is required to read nsx.lock", file=sys.stderr)
    raise SystemExit(2)


def git(repo: Path, *args: str) -> str | None:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo), *args],
            capture_output=True, text=True, check=True,
        )
    except (subprocess.CalledProcessError, OSError):
        return None
    return out.stdout.strip()


def is_own_checkout(path: Path) -> bool:
    """True only if `path` is the root of its own git checkout.

    `git -C modules/foo rev-parse HEAD` happily answers with the *app* repo's
    HEAD when modules/foo carries no git metadata, which would look like a
    passing check. Require the toplevel to be the module directory itself.
    """
    if not (path / ".git").exists():
        return False
    top = git(path, "rev-parse", "--show-toplevel")
    if top is None:
        return False
    try:
        return Path(top).resolve() == path.resolve()
    except OSError:
        return False


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: verify_modules.py <repo-dir> <board>", file=sys.stderr)
        return 2
    repo, board = Path(argv[1]), argv[2]

    lock_path = repo / "nsx.lock"
    if not lock_path.is_file():
        print(f"error: {lock_path} not found", file=sys.stderr)
        return 2
    lock = yaml.safe_load(lock_path.read_text())

    target = (lock.get("targets") or {}).get(board)
    if not target:
        print(f"error: nsx.lock has no entry for board {board}", file=sys.stderr)
        return 1

    # Collapse the logical module list onto the directories it vendors into.
    # Several logical modules legitimately share one checkout (the whole
    # nsx-ambiq-sdk family), so only *differing* commits are ambiguous.
    pinned: dict[str, set[str]] = {}
    hashes: dict[str, set[str]] = {}
    for entry in (target.get("modules") or {}).values():
        resolved = entry.get("resolved") or {}
        vendored = resolved.get("vendored_at")
        if not vendored or not str(vendored).startswith("modules/"):
            continue
        if resolved.get("commit"):
            pinned.setdefault(vendored, set()).add(resolved["commit"])
        if resolved.get("content_hash"):
            hashes.setdefault(vendored, set()).add(resolved["content_hash"])

    lines: list[str] = []
    refusals: list[str] = []
    warnings: list[str] = []

    modules_dir = repo / "modules"
    on_disk = sorted(p for p in modules_dir.iterdir() if p.is_dir()) if modules_dir.is_dir() else []

    for path in on_disk:
        rel = f"modules/{path.name}"
        commits = pinned.get(rel, set())
        digests = hashes.get(rel, set())

        if not commits and digests:
            # `kind: vendored` module, committed in this repo. There is no
            # upstream commit to pin; the lock pins the directory content and
            # `nsx lock --check` is what detects drift.
            lines.append(f"  {rel:<28} in-tree, {next(iter(digests)) if len(digests) == 1 else 'unknown'}")
            continue

        if not commits:
            warnings.append(f"{rel} is on disk but not pinned in nsx.lock for {board}")
            lines.append(f"  {rel:<28} NOT IN LOCK")
            continue

        if len(commits) > 1:
            warnings.append(
                f"{rel} is pinned to multiple commits in nsx.lock "
                f"({', '.join(sorted(c[:12] for c in commits))}); recording without enforcing"
            )
            lines.append(f"  {rel:<28} AMBIGUOUS PIN")
            continue

        commit = next(iter(commits))
        digest = next(iter(digests)) if len(digests) == 1 else "unknown"

        if is_own_checkout(path):
            head = git(path, "rev-parse", "HEAD")
            status = git(path, "status", "--porcelain")
            dirty = bool(status)
            if head != commit:
                refusals.append(
                    f"{rel} HEAD {head} does not match the nsx.lock pin {commit}"
                )
            if dirty:
                refusals.append(f"{rel} has uncommitted changes")
            state = "clean" if not dirty else "DIRTY"
            lines.append(f"  {rel:<28} git {(head or 'unknown')[:12]} {state} (pin {commit[:12]})")
        else:
            # Vendored source drop: no git metadata, so HEAD cannot be checked
            # independently. `nsx configure --frozen` is what detects drift here.
            lines.append(f"  {rel:<28} vendored, pin {commit[:12]}, {digest}")

    missing = sorted((set(pinned) | set(hashes)) - {f"modules/{p.name}" for p in on_disk})
    for rel in missing:
        refusals.append(f"{rel} is pinned in nsx.lock but missing from disk")

    report = ["Dependency modules (from nsx.lock, target %s)" % board,
              "-" * 52, *lines]
    if not any(is_own_checkout(p) for p in on_disk):
        report += [
            "",
            "  All modules are vendored source drops without git metadata, so",
            "  their HEAD cannot be confirmed independently here. Drift is",
            "  caught by `nsx configure --frozen`, which this build ran.",
        ]
    for w in warnings:
        report.append(f"  WARNING: {w}")

    print("\n".join(report))

    if refusals:
        print("\nerror: refusing to package:", file=sys.stderr)
        for r in refusals:
            print(f"  - {r}", file=sys.stderr)
        return 1
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
