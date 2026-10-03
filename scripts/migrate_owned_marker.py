#!/usr/bin/env python3
"""Compiler-guided migration to the v0.169 call-site transfer marker (#1348).

Spec §22.3 (v0.169): when the argument to an `owned` parameter is a NAMED OWNED
BINDING (an owned local or an `owned` parameter — the cases the call leaves
moved-from), the caller must write the marker: `M(owned x)`. Fresh producers
(`new`, a call returning `owned T`, an operator result, `move obj.f`) stay
unmarked.

This script drives `bezelc` over one or more Bezel source files and, for every
"this call consumes `x`; write `M(owned x)`" diagnostic, inserts `owned ` before
the argument at the reported (line, column). It re-runs until the file is free of
missing-marker diagnostics (each pass may surface a fresh site once the earlier
one no longer masks it in overload/flow analysis).

It ONLY acts on the missing-marker diagnostic; the spurious-marker diagnostics
(borrow / fresh producer / non-owned position) are left for a human to fix,
because they signal that the source already had a wrong marker, not a mechanical
omission.

Usage:
    scripts/migrate_owned_marker.py --bezelc build/bezelc FILE.bzl [FILE.bzl ...]
    scripts/migrate_owned_marker.py --bezelc build/bezelc --glob 'tests/**/*.bzl'

The bezelc invocation is memory-caged with systemd-run (matching the project's
build/run discipline) unless --no-cage is given.
"""

import argparse
import glob as globmod
import re
import shlex
import subprocess
import sys

# `FILE:LINE:COL: this call consumes `NAME`; write `M(owned NAME)` ...`
DIAG_RE = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+): "
    r"this call consumes `(?P<name>[^`]+)`; write `M\(owned "
)


def cage(cmd, use_cage):
    if not use_cage:
        return cmd
    return [
        "systemd-run", "--user", "--scope", "-q",
        # Never app.slice, the desktop's own CPU group (2026-10-03 freeze).
        "-p", "Slice=bezel-heavy.slice",
        "-p", "MemoryMax=20G", "-p", "MemorySwapMax=0",
    ] + cmd


def compile_once(bezelc, path, use_cage, extra_args):
    """Compile `path`, returning the combined diagnostics text."""
    cmd = cage([bezelc, "-c", "-o", "/dev/null", *extra_args, path], use_cage)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.stdout + proc.stderr


def missing_marker_sites(diag_text, path):
    """Extract (line, col) insertion points for this file's missing markers."""
    sites = []
    for ln in diag_text.splitlines():
        m = DIAG_RE.match(ln)
        if not m:
            continue
        # The diagnostic's file may be an absolute/relative variant of `path`;
        # match on basename so a temp/wrapped path still maps back.
        if m.group("file").split("/")[-1] != path.split("/")[-1]:
            continue
        sites.append((int(m.group("line")), int(m.group("col"))))
    return sites


def apply_sites(path, sites):
    """Insert `owned ` at each (1-based line, 1-based col). Rightmost first so
    earlier insertions on the same line do not shift later columns."""
    with open(path, "r") as f:
        lines = f.readlines()
    # Group by line; within a line insert from the largest column downwards.
    by_line = {}
    for (ln, col) in sites:
        by_line.setdefault(ln, []).append(col)
    for ln, cols in by_line.items():
        if ln - 1 >= len(lines):
            continue
        text = lines[ln - 1]
        for col in sorted(cols, reverse=True):
            idx = col - 1
            if idx < 0 or idx > len(text):
                continue
            # Idempotence guard: skip if `owned ` already precedes this position.
            if text[max(0, idx - 6):idx] == "owned ":
                continue
            text = text[:idx] + "owned " + text[idx:]
        lines[ln - 1] = text
    with open(path, "w") as f:
        f.writelines(lines)


def migrate_file(bezelc, path, use_cage, extra_args, max_passes=25):
    total = 0
    for _ in range(max_passes):
        diag = compile_once(bezelc, path, use_cage, extra_args)
        sites = missing_marker_sites(diag, path)
        if not sites:
            return total, diag
        apply_sites(path, sites)
        total += len(sites)
    # Final compile to report residual state.
    diag = compile_once(bezelc, path, use_cage, extra_args)
    return total, diag


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bezelc", required=True, help="path to the bezelc binary")
    ap.add_argument("--glob", action="append", default=[],
                    help="glob of files to migrate (repeatable)")
    ap.add_argument("--no-cage", action="store_true",
                    help="do not wrap bezelc in systemd-run memory cage")
    ap.add_argument("--arg", action="append", default=[],
                    help="extra bezelc argument (e.g. --arg --import=foo.o)")
    ap.add_argument("files", nargs="*", help="files to migrate")
    args = ap.parse_args()

    files = list(args.files)
    for g in args.glob:
        files.extend(sorted(globmod.glob(g, recursive=True)))
    if not files:
        print("no input files", file=sys.stderr)
        return 1

    use_cage = not args.no_cage
    grand = 0
    for path in files:
        added, diag = migrate_file(args.bezelc, path, use_cage, args.arg)
        residual = missing_marker_sites(diag, path)
        grand += added
        status = "OK" if not residual else f"RESIDUAL({len(residual)})"
        if added or residual:
            print(f"[{status}] {path}: inserted {added} marker(s)")
    print(f"total markers inserted: {grand}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
