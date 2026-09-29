#!/usr/bin/env python3
# gen_internal_disposition_snapshot.py — build-free Family K generator.
#
# WHAT THIS SCRIPT IS FOR.  defect-campaign §2.7 (Family K) named the
# "internal:" Warning sites as the family anchor; tests/support/
# internal_degrade_disposition_snapshot_2924_test.cpp scans src/codegen/
# at HEAD, builds a Severity resolution per site, and asserts the WARNING
# set is exactly the 22 entries the test file's kWarningDegrades[] array
# holds.  The test runs at every lane build; this script does the same
# scan and emits a deterministic fixture file a human can diff against.
#
# WHY A SEPARATE SCRIPT.  The test file is C++ (and must be — it lives
# alongside the gtest suite), so re-running it from the shell is heavyweight.
# The Python mirror of the scan lets a developer:
#
#   * double-check the test's hand-coded disposition table is current,
#     by diffing the script's emitted fixture against the table;
#   * audit WHICH severity each `internal:` site resolves to without
#     rebuilding;
#   * verify the scan is deterministic — running it twice on the same
#     tree must produce byte-identical output.
#
# DETERMINISM.  Same input tree + same Python version = same output.  No
# timestamps, no random IDs, no environment variables consulted.  The
# scan walks files in sorted order, the site scan within each file is
# line-by-line forward, the severity probe walks BACKWARD 40 lines — so
# both the order of sites and the order of severity candidates is
# stable.
#
# WHAT THIS SCRIPT IS NOT.  It does NOT update the kWarningDegrades[]
# table; that's a deliberate human edit (the disposition is a per-site
# judgement, not a regex match, per the proposal's "Opus-level review"
# paragraph).  The fixture is a DERIVED artifact — the source of truth
# remains the C++ array.
#
# USAGE.  python3 scripts/gen_internal_disposition_snapshot.py [out.txt]
#         (default out.txt is stdout).  The fixture format is one row per
#         site: SEVERITY  FILE:LINE  <message text, semicolon-terminated>.
#
# Companion to tests/support/internal_degrade_disposition_snapshot_2924_
# test.cpp and tests/support/internal_degrade_disposition_members_2924_
# test.cpp.  Defect-campaign §2.7 (Family K), issue #2924.

from __future__ import annotations

import os
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
SRC_ROOT = REPO_ROOT / "src"

# Mirror of the C++ test's literal ("internal: ") needle.  Every site in
# src/codegen/*.cpp is a `report(Severity::X, loc, "internal: …")` call;
# the literal is what marks the site as one Family K considers.
NEEDLE = '"internal: '

# Severity is taken from the NEAREST preceding `Severity::<name>` token on
# the same or any of the 40 lines above the site.  The C++ test walks
# backward 40 lines for the same reason — the argument list routinely
# wraps, putting the severity several lines before the literal.
SEV_RE = re.compile(r"Severity::([A-Za-z_][A-Za-z0-9_]*)")
INTERNAL_RE = re.compile(r'"internal:\s')


def _is_comment_line(line: str) -> bool:
    """A `//` or `/* */` line is not a site — same heuristic as the C++ test."""
    stripped = line.lstrip()
    return stripped.startswith("//") or stripped.startswith("/*") or \
           stripped.startswith("*")


def _scan_file(rel: Path, text: str, out: list[tuple[str, str, int, str]]) -> None:
    """Append one tuple per site to `out`.  Tuple is
    (severity, file, line_no, full_text_to_semicolon)."""
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if NEEDLE not in line:
            continue
        if _is_comment_line(line):
            continue
        # Forward to the end of the report(...) statement.  The terminator
        # test is "the trimmed line ENDS with `;`", not "contains a `;`":
        # some messages contain a semicolon INSIDE a string literal.
        site_text = ""
        for j in range(i, min(i + 40, len(lines))):
            site_text += lines[j] + " "
            stripped = lines[j].rstrip()
            if stripped.endswith(";"):
                break

        # Backward scan for the nearest `Severity::` token within 40 lines
        # above the site (skipping comment lines).
        severity = ""
        for j in range(i, max(-1, i - 40), -1):
            candidate = lines[j]
            if _is_comment_line(candidate):
                continue
            m = SEV_RE.search(candidate)
            if m:
                severity = m.group(1)
                break
        out.append((severity, str(rel), i + 1, site_text.rstrip()))


def _walk_src(root: Path) -> list[tuple[str, str, int, str]]:
    """Recursive walk of root for .cpp/.hpp, returning sorted sites.

    Sorted by (severity, file, line) so the output is byte-stable across
    runs (the OS may return files in any order; sort fixes that).  Within
    a file the scan is line-by-line forward, so per-file ordering is
    already deterministic."""
    sites: list[tuple[str, str, int, str]] = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in sorted(filenames):
            if not (fn.endswith(".cpp") or fn.endswith(".hpp")):
                continue
            abs_path = Path(dirpath) / fn
            rel_path = abs_path.relative_to(REPO_ROOT)
            try:
                text = abs_path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            _scan_file(rel_path, text, sites)
    sites.sort(key=lambda s: (s[0], s[1], s[2]))
    return sites


def _emit(sites: list[tuple[str, str, int, str]], out) -> int:
    """Write the fixture file to `out`.  Returns the number of WARNING
    sites (the population the snapshot test pins)."""
    n_warning = 0
    n_error = 0
    n_other = 0
    for sev, rel, line, text in sites:
        if not sev:
            sev_disp = "UNCLASSIFIED"
            n_other += 1
        elif sev == "Warning":
            sev_disp = "Warning"
            n_warning += 1
        elif sev == "Error":
            sev_disp = "Error"
            n_error += 1
        else:
            sev_disp = sev  # Note etc.
            n_other += 1
        out.write(f"{sev_disp:<11}  {rel}:{line}  {text}\n")
    out.write(
        f"\n# {len(sites)} sites total: {n_warning} Warning, "
        f"{n_error} Error, {n_other} other/unclassified\n"
    )
    return n_warning


def main(argv: list[str]) -> int:
    if len(argv) > 2:
        print(f"usage: {argv[0]} [out.txt]", file=sys.stderr)
        return 2
    out_path = Path(argv[1]) if len(argv) == 2 else None
    if out_path is None:
        emit = _emit(_walk_src(SRC_ROOT), sys.stdout)
    else:
        with out_path.open("w", encoding="utf-8") as f:
            emit = _emit(_walk_src(SRC_ROOT), f)
    # Exit 0 always — this is a generator, not a test.  A test that
    # compares this output to a committed fixture belongs in the C++
    # suite (the existing snapshot test).
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))