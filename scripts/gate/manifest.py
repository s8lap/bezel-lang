"""manifest.py — read build/tests/test_binaries.txt (Phase 5, #3010).

Python port of the manifest-reading inline bash in gate.sh / run_parallel.sh /
agent_check.sh (they `IFS=$'\\t' read -r group path count` the file in several
places). The `verify` subcommand is WIRED INTO gate.sh (Wave 3): it reads the
manifest AND checks every listed binary is executable, hard-failing on
missing/non-executable (#3011 — the gate's own input was the defect it catches).
The pure `binaries` listing remains for consumers that have not built yet.

THE MANIFEST (tests/CMakeLists.txt:16-17, 242-246): one row per per-area test
binary, `<group>\\t<abs path>\\t<#sources>`, largest group first. It is the
single source of truth every runner reads — a binary can never be silently
omitted because the set a runner accounts for is generated from the very list
CMake built (run_parallel.sh:42-57). Never hardcode a binary path; never
enumerate the areas by hand.

THE PARTITION INVARIANTS (Plan-A 1.13; tests/CMakeLists.txt:25-44). The suite is
the project's correctness oracle, so the union of the per-area binaries must be
EXACTLY the old single binary's test set. Two configure-time FATAL_ERRORs
enforce a TRUE partition:
  (1) COMPLETENESS — the per-group source lists cover one flat glob of every
      *.cpp under the claimed dirs; a source in NO group, or in TWO, is fatal.
  (2) NO UNCLAIMED AREA — any tests/<dir>/ holding a *.cpp no group claims is
      fatal (a brand-new tests/<area>/ would otherwise be silently invisible).

`partition_is_complete` is a read-time DEFENSE over invariant (1): the manifest
carries only the per-group COUNT (not per-file paths), so the full file-level
check stays in CMake (which has the per-group source lists). But the COUNT is
enough to catch a broken partition at the aggregate: a source in two groups
inflates the sum (a double-count); a source in no group, or an unclaimed dir,
deflates it (the glob sees sources the manifest does not). Either mismatch fails
the check loudly — a hand-edited manifest or a build dir whose CMakeCache drifted
from the sources must not silently shrink the suite.

Stdlib only (no pytest). See test_manifest.py for the read + partition pins.
"""

import argparse
import os
import sys


class Binary:
    """One row of build/tests/test_binaries.txt: a per-area test binary.

    `group` is the area name (codegen/ownership/sema/frontend/module/support),
    `path` the absolute binary path, `num_sources` the # of *.cpp that link in.
    """

    __slots__ = ("group", "path", "num_sources")

    def __init__(self, group, path, num_sources):
        self.group = group
        self.path = path
        self.num_sources = int(num_sources)

    def __repr__(self):
        return "Binary(group=%r, path=%r, num_sources=%d)" % (
            self.group, self.path, self.num_sources)

    def __eq__(self, other):
        return (
            isinstance(other, Binary)
            and self.group == other.group
            and self.path == other.path
            and self.num_sources == other.num_sources
        )

    def __hash__(self):
        return hash((self.group, self.path, self.num_sources))


def read_manifest(path):
    """Read build/tests/test_binaries.txt into a list[Binary].

    Each row is `<group>\\t<abs path>\\t<#sources>` (tests/CMakeLists.txt:242-243;
    run_parallel.sh:53 parses it with `IFS=$'\\t' read -r n p w`). Blank lines are
    skipped. A malformed row (wrong field count, empty group/path, non-integer or
    negative #sources) raises ValueError — a manifest a consumer cannot trust is
    FATAL, never silently truncated (the #3000 shape: plausible-looking garbage a
    reader proceeds on). An EMPTY manifest raises ValueError too: per the
    FATAL_ERROR spirit (tests/CMakeLists.txt:26-29) an empty suite would silently
    vanish the gate's oracle. A MISSING file raises FileNotFoundError for the
    caller to handle (gate.sh / run_parallel.sh refuse preflight with a message).
    """
    out = []
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            fields = line.split("\t")
            if len(fields) != 3:
                raise ValueError(
                    "%s:%d: expected 3 tab-separated fields "
                    "(<group>\\t<path>\\t<#sources>), got %d — %r"
                    % (path, lineno, len(fields), line)
                )
            group, bpath, nsources = fields
            if not group or not bpath:
                raise ValueError(
                    "%s:%d: empty group or path field — %r"
                    % (path, lineno, line)
                )
            try:
                n = int(nsources)
            except ValueError:
                raise ValueError(
                    "%s:%d: #sources is not an integer — %r"
                    % (path, lineno, line)
                )
            if n < 0:
                raise ValueError(
                    "%s:%d: negative #sources — %r" % (path, lineno, line)
                )
            out.append(Binary(group, bpath, n))
    if not out:
        # per tests/CMakeLists.txt:26-29: an empty manifest means the suite would
        # silently vanish — a gate must NEVER proceed on zero binaries.
        raise ValueError("%s: manifest is empty — no test binaries" % path)
    return out


def partition_is_complete(manifest, tests_glob, num_shared_entry=1):
    """Read-time defense over partition invariant (1). per Plan-A 1.13.

    The manifest carries only the per-group COUNT, so this checks the aggregate
    consistency a count can catch (the full per-file partition stays in CMake,
    which has the per-group source lists — tests/CMakeLists.txt:125-148):

      - every binary declares num_sources > 0 (no empty/phantom group)
      - group names are unique (no two binaries claim the same group)
      - paths are unique (no duplicate binary)
      - the sum of num_sources == len(tests_glob) - num_shared_entry

    The last line is the union invariant in count form. `tests_glob` is the flat
    collection of EVERY tests/**/*.cpp on disk, INCLUDING the shared entry point
    (test_main.cpp). The per-group counts EXCLUDE that entry point — it compiles
    into every binary and is not part of any one group's partition
    (tests/CMakeLists.txt:71-74, 109, 138) — so the manifest's total is one
    short of the flat glob. `num_shared_entry` defaults to 1 (the single
    test_main.cpp); pass 0 if the glob already excludes it.

    A source claimed by two groups inflates the sum (double-count ⇒ sum too big);
    a source in no group, or an unclaimed tests/<dir>/, deflates it (the glob sees
    sources the manifest does not ⇒ sum too small). Either ⇒ False.
    """
    if not manifest:
        return False
    groups = set()
    paths = set()
    total = 0
    for b in manifest:
        if b.num_sources <= 0:
            return False
        if b.group in groups:
            return False  # a group claimed by two binaries — per #3011 the
            # missing-binary guard's sibling: the partition must be a true split.
        if b.path in paths:
            return False  # duplicate binary path
        groups.add(b.group)
        paths.add(b.path)
        total += b.num_sources
    glob_n = len(tests_glob)
    return total == glob_n - num_shared_entry


class VerifyError(Exception):
    """A manifest binary is missing or not executable (#3011)."""


def verify_binaries(path, *, is_executable=None):
    """Read the manifest and verify every listed binary is executable (#3011).

    The gate.sh port of the old `cut -f2` listing + the per-binary
    `[ -x "$b" ] || fail` loop. A binary the manifest lists but that is NOT
    executable (missing, never built, renamed, or its +x lost) is a HARD
    failure: the old loop hard-failed on exactly this, and skipping the binary
    would silently LOWER the bar by however many tests it holds (the #3011
    variant-3 shape — the floor's own input was the defect it catches).

    Returns the binary paths in manifest order on success — the same paths
    `cut -f2` produces on a well-formed manifest (the dual-run assertion in
    gate.sh pins this equivalence). Raises VerifyError on the first
    non-executable binary; read_manifest's ValueError propagates on a
    malformed/empty manifest. `is_executable` is an injection seam for the unit
    tests (default: exists + X_OK, matching coverage.py:159 — the same `[ -x ]`
    semantics the old bash loop used).
    """
    if is_executable is None:
        is_executable = lambda p: os.path.exists(p) and os.access(p, os.X_OK)
    binaries = read_manifest(path)
    for b in binaries:
        if not is_executable(b.path):
            raise VerifyError(
                "%s: test binary '%s' (group %s) is missing or not executable "
                "— build reported success but a per-area binary was not "
                "produced (#3011: never silently lower the bar)"
                % (path, b.path, b.group))
    return [b.path for b in binaries]


# ---------------------------------------------------------------------------
# CLI — gate.sh / run_parallel.sh / agent_check.sh call
# `python3 -m gate.manifest binaries ...` to enumerate the binary paths the way
# the old `cut -f2 build/tests/test_binaries.txt` did. read_manifest's strict
# validation replaces the silent skip the loose bash cut performed, so a
# malformed row now fails loudly instead of dropping a binary from the runner's
# accounting.
#
# `python3 -m gate.manifest verify ...` (Wave 3) ADDS the #3011 executability
# check on top of the listing — it is the gate.sh replacement for `cut -f2` +
# the per-binary `[ -x ]` loop, hard-failing on a missing/non-executable binary.
# ---------------------------------------------------------------------------

def _main_binaries(argv):
    """`binaries [--path P]` → one binary path per line."""
    p = argparse.ArgumentParser(prog="manifest.py binaries")
    p.add_argument("--path", default="build/tests/test_binaries.txt")
    args = p.parse_args(argv)
    for b in read_manifest(args.path):
        print(b.path)
    return 0


def _main_verify(argv):
    """`verify [--path P]` → check each binary is executable; print paths.

    The gate.sh replacement for `cut -f2` + the per-binary `[ -x ]` loop
    (#3011). Prints the binary paths (one per line, manifest order) on success;
    exits nonzero with a stderr message if any binary is missing/non-executable
    (never silently lower the bar) or the manifest is malformed/empty. Emits
    NOTHING to stdout on failure so a consumer piping the output never sees a
    partial set — the gate fails before iterating a truncated listing.
    """
    p = argparse.ArgumentParser(prog="manifest.py verify")
    p.add_argument("--path", default="build/tests/test_binaries.txt")
    args = p.parse_args(argv)
    try:
        paths = verify_binaries(args.path)
    except VerifyError as exc:
        sys.stderr.write("manifest.py verify: %s\n" % exc)
        return 1
    # read_manifest's ValueError (malformed/empty manifest) propagates as a
    # traceback — still a hard failure (nonzero exit), matching `binaries`.
    for bpath in paths:
        print(bpath)
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "binaries":
        return _main_binaries(argv[2:])
    if len(argv) >= 2 and argv[1] == "verify":
        return _main_verify(argv[2:])
    sys.stderr.write("usage: manifest.py {binaries|verify} [--path PATH]\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
