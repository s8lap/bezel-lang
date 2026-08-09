"""coverage.py — the coverage floor (Phase 4 module, #3002 / #3011).

THE DEFECT THIS EXISTS TO CLOSE. On 2026-08-07 a plain gate attempt reported
5156 of 16482 gtests (31%), every crashed shard was a timeout victim, flake
arbitration cleared them all (a suite killed at the ceiling ALWAYS passes when
re-run alone — its process was terminated, it never failed), and gate.sh
printed `== GATE PASS ==` with rc=0 and no warning. 11,326 tests never ran and
the gate said pass. The only thing that distinguishes a complete run from a
truncated one is the number that SHOULD have run — which the gate never asked
for. So this module asks. `BEZEL_TEST_ENUMERATE=1 <binary>` lists what a
process WOULD run without running it (~30ms per binary), and the
CMake-generated `test_binaries.txt` manifest's partition of tests/**/*.cpp is
enforced by two FATAL_ERROR invariants — so this sum is exactly the whole
suite, by construction, and cannot drift as areas are added.

#3011 — THE FLOOR'S OWN INPUT WAS THE DEFECT IT WAS WRITTEN TO CATCH. The first
bash version skipped a manifest row with `[ -x "$bin" ] || continue`, so a
binary that failed to link / was never built / was renamed did not fail the
gate — it silently LOWERED the bar it was supposed to raise. Miss the codegen
binary and `expected` drops from 16482 to 5156, which is exactly the number the
run that motivated #3002 reported as a pass. That is variant 3 of this
project's category-defect taxonomy — a correct predicate fed by a source
collection that never held the population — sitting inside the fix for a
variant-3 defect. So: a missing binary, an empty registry, or an enumerate==0
are each a HARD failure that NEVER lowers the bar.

This is a FOUNDATION module (Wave 1): written + unit-tested, NOT yet wired
into gate.sh (Wave 2 replaces the bash block at gate.sh:882-973 with a call
here). gate.sh is untouched on this lane. The two public entry points mirror
the bash's two phases — enumerate the registry, then compare — so the Wave-2
wiring is a near-mechanical substitution.

Stdlib only (no pytest). See test_coverage.py for the load-bearing pins:
  - missing binary => HARD fail (#3011 — never a smaller expectation)
  - zero manifest rows => HARD fail
  - enumerate==0 => HARD fail
  - ran < expected => coverage-short (exit 3, retry — NOT a real failure)
  - ran >= expected => ok (a superset is fine: == is too strict)
"""

import argparse
import os
import pathlib
import subprocess
import sys
from dataclasses import dataclass, field

# per #3002: a run that reports fewer gtests than the registry says exist is a
# TRUNCATED run, and a pass over a truncated run is not a pass. Exit 3 = NO
# TRUSTWORTHY VERDICT (gate_full_cycle.sh retries on 3), not 1 = real failure —
# a short run says nothing about the tree either way, and truncation is usually
# transient (contention, a wedged shard).
EXIT_OK = 0
EXIT_RETRY = 3  # coverage-short OR registry-unusable — both retry, neither is PASS


@dataclass
class BinaryRow:
    """One manifest row after enumeration.

    `count` is the BEZEL_TEST_ENUMERATE line count; it is 0 for a row whose
    binary could not be enumerated (not executable, or enumerated nothing).
    `note` is "" when the row is usable, else the reason it is not.
    """
    group: str        # the manifest's first field, e.g. "codegen"
    path: str         # the manifest's second field — the binary path
    count: int        # enumerated test count; 0 when enumeration failed
    executable: bool  # False => the registry lists a binary that is not runnable
    note: str         # "" ok; else "not executable" / "enumerated 0 tests"


@dataclass
class EnumerateResult:
    """The registry's expectation, with its own structural validity attached.

    `usable` is the #3011 invariant materialized: it is True ONLY when the
    manifest was readable, listed at least one row, and EVERY row's binary was
    executable AND enumerated a positive count. Any gap makes the floor
    UNUSABLE — the comparison must not run against a secretly-lowered bar, so
    `total_expected` is reported but the verdict path treats unusable as a hard
    fail regardless of how it compares to `ran`.
    """
    rows: int                          # manifest rows seen (0 => unreadable/empty)
    total_expected: int                # sum of counts over USABLE rows only
    per_binary_expected: dict = field(default_factory=dict)  # {group: count}
    binaries: list = field(default_factory=list)             # BinaryRow, in order
    usable: bool = True
    errors: list = field(default_factory=list)               # human-readable lines


@dataclass
class FloorVerdict:
    """The coverage-floor decision. `verdict` names the outcome; `exit_code`
    is what gate.sh exits with for that outcome."""
    ok: bool                 # True ONLY for "ok"
    verdict: str             # "ok" | "coverage-short" | "registry-unusable"
    exit_code: int           # 0 for ok; 3 for both failure shapes
    ran: int
    expected: int
    per_binary_ran: dict = field(default_factory=dict)
    per_binary_expected: dict = field(default_factory=dict)
    message: str = ""


def _count_enumerate_lines(stdout):
    """Count test names the way `wc -l` does (one suite.name per line).

    Faithful to gate.sh:934 `BEZEL_TEST_ENUMERATE=1 "$bin" | wc -l`: the
    enumerate protocol prints exactly one test per line, each terminated by a
    newline, so counting newline bytes is counting tests. The exit code of the
    binary is deliberately IGNORED (the bash pipes through `wc` and never tests
    `$?`): a binary that crashes after printing 5 lines counts 5, and a binary
    that prints nothing counts 0 (which then hard-fails #3011).
    """
    if not stdout:
        return 0
    return stdout.count("\n")


def _default_enumerate_runner(binary):
    """Run `BEZEL_TEST_ENUMERATE=1 <binary>` and return (rc, stdout_text).

    Injected in tests; in production this is the one subprocess-per-binary the
    plan names. stderr is dropped (bash: `2>/dev/null`) so a noisy/crashing
    binary does not corrupt the line count. A 60s ceiling bounds a wedged
    binary — the enumerate path does no real work, ~30ms is typical.
    """
    env = dict(os.environ)
    env["BEZEL_TEST_ENUMERATE"] = "1"
    try:
        proc = subprocess.run(
            [binary], env=env, capture_output=True, text=True, timeout=60)
        return proc.returncode, proc.stdout
    except subprocess.TimeoutExpired:
        return 124, ""
    except OSError:
        # A binary that cannot be launched at all (ENOENT etc.) yields no
        # output; the caller's executable check is the primary guard, but a
        # vanished-between-check-and-run race degrades to count 0 here.
        return 127, ""


def enumerate_expected(manifest_path, *, runner=None, is_executable=None):
    """Read the manifest and enumerate every binary's test count.

    A faithful port of gate.sh:919-950. For each row `<group>\\t<path>\\t<n>`:
      - require the binary to be EXECUTABLE (#3011: a non-executable binary is
        a HARD failure — skipping it would silently lower the bar);
      - run `BEZEL_TEST_ENUMERATE=1 <binary>` and count the lines;
      - require the count to be positive (#3011: enumerate==0 is a HARD
        failure — the registry read failed).
    Returns an EnumerateResult whose `usable` is False if ANY row failed, the
    manifest is unreadable, or it lists zero rows. `runner` and `is_executable`
    are injection seams for the unit tests.
    """
    if runner is None:
        runner = _default_enumerate_runner
    if is_executable is None:
        is_executable = lambda p: os.path.exists(p) and os.access(p, os.X_OK)

    result = EnumerateResult(rows=0, total_expected=0, usable=True, errors=[])

    p = pathlib.Path(manifest_path)
    if not p.is_file():
        # per #3011: an unreadable manifest cannot establish what SHOULD run,
        # so the floor is unusable (never silently skipped). gate.sh:943-945.
        result.usable = False
        result.errors.append(
            "GATE FAILURE: {} is unreadable — cannot establish what SHOULD run"
            .format(manifest_path))
        return result

    for raw in p.read_text(errors="replace").splitlines():
        if not raw.strip():
            continue
        parts = raw.split("\t")
        if len(parts) < 2:
            # A malformed row is treated like a missing binary: refuse to
            # guess rather than silently dropping it from the population.
            result.rows += 1
            result.usable = False
            result.errors.append(
                "GATE FAILURE: test registry row is malformed: {!r}".format(raw))
            continue
        group, binpath = parts[0], parts[1]
        result.rows += 1

        if not is_executable(binpath):
            # per #3011: NEVER `[ -x ] || continue`. A binary the registry
            # lists but that is not executable is a HARD failure — skipping it
            # lowers the coverage floor by however many tests it holds, which
            # is the exact defect #3002 exists to prevent. gate.sh:927-933.
            result.binaries.append(BinaryRow(
                group=group, path=binpath, count=0, executable=False,
                note="not executable"))
            result.usable = False
            result.errors.append(
                "GATE FAILURE: test registry lists '{}' ({}) but it is not "
                "executable\n##   Skipping it would LOWER the coverage floor "
                "by however many tests it holds,\n##   which is the exact "
                "defect issue 3002 exists to prevent. Refusing to guess."
                .format(binpath, group))
            continue

        _rc, stdout = runner(binpath)
        n = _count_enumerate_lines(stdout)
        if n <= 0:
            # per #3011: enumerate==0 is a HARD failure (the registry read
            # failed), never a 0-count expectation. gate.sh:935-939.
            result.binaries.append(BinaryRow(
                group=group, path=binpath, count=0, executable=True,
                note="enumerated 0 tests"))
            result.usable = False
            result.errors.append(
                "GATE FAILURE: '{}' ({}) enumerated 0 tests — the registry "
                "read failed".format(binpath, group))
            continue

        result.binaries.append(BinaryRow(
            group=group, path=binpath, count=n, executable=True, note=""))
        result.total_expected += n
        result.per_binary_expected[group] = (
            result.per_binary_expected.get(group, 0) + n)

    if result.rows == 0:
        # per #3011: a registry listing ZERO binaries is a HARD failure — the
        # manifest is empty/missing the population, not "zero tests expected".
        # gate.sh:947-950.
        result.usable = False
        result.errors.append(
            "GATE FAILURE: the test registry lists ZERO binaries")

    return result


def floor_ok(ran, expected, per_binary_ran, per_binary_expected,
             *, usable=True, errors=None):
    """Compare what RAN against what the registry says SHOULD run.

    A faithful port of gate.sh:951-973. Three outcomes:
      - registry-unusable: the manifest could not be read completely (a binary
        missing, a row malformed, enumerate==0, zero rows, file unreadable).
        HARD fail (exit 3). The #3011 invariant — never silently lower the bar.
      - coverage-short: the registry was usable but `ran < expected`. Exit 3
        (RETRY — a short run says nothing about the tree; truncation is
        usually transient). gate_full_cycle.sh retries on 3.
      - ok: `ran >= expected`. A SUPERSET is fine (== would be too strict — a
        re-run that picks up a few extra tests is not a defect); exit 0.

    `usable`/`errors` carry the #3011 structural validity from
    enumerate_expected(); they default to "usable" so a caller that validated
    the registry itself can call floor_ok as a pure comparison.
    """
    if not usable:
        msg = "coverage floor unusable: the registry could not be read completely"
        if errors:
            msg = "\n".join(["## " + e for e in errors] + ["== GATE FAIL: " + msg])
        return FloorVerdict(
            ok=False, verdict="registry-unusable", exit_code=EXIT_RETRY,
            ran=ran, expected=expected,
            per_binary_ran=dict(per_binary_ran or {}),
            per_binary_expected=dict(per_binary_expected or {}),
            message=msg)

    if ran < expected:
        # per #3002: COVERAGE SHORT. What must never happen again is `== GATE
        # PASS ==` over 31% coverage. Exit 3 (retry), not 1 (real fail).
        pct = (ran * 100 // expected) if expected > 0 else 0
        msg = (
            "## GATE FAILURE: COVERAGE SHORT — {} of {} gtests ran ({}%)\n"
            "##   The registry says {} tests exist; this run reported {}.\n"
            "##   A pass over a truncated run is not a pass, however the "
            "missing shards were arbitrated.\n"
            "== GATE FAIL: coverage short: {} of {} gtests ran"
            .format(ran, expected, pct, expected, ran, ran, expected))
        return FloorVerdict(
            ok=False, verdict="coverage-short", exit_code=EXIT_RETRY,
            ran=ran, expected=expected,
            per_binary_ran=dict(per_binary_ran or {}),
            per_binary_expected=dict(per_binary_expected or {}),
            message=msg)

    return FloorVerdict(
        ok=True, verdict="ok", exit_code=EXIT_OK,
        ran=ran, expected=expected,
        per_binary_ran=dict(per_binary_ran or {}),
        per_binary_expected=dict(per_binary_expected or {}),
        message="== gate: coverage floor OK — {} of {} gtests "
                "(registry-complete) ==".format(ran, expected))


def check_coverage_floor(ran, per_binary_ran, manifest_path,
                         *, runner=None, is_executable=None):
    """The full coverage-floor check: enumerate the registry, then compare.

    This is the Wave-2 entry point gate.sh will call (replacing the bash block
    at :882-973). It composes enumerate_expected + floor_ok so the #3011
    structural validity flows straight into the verdict.
    """
    enum = enumerate_expected(
        manifest_path, runner=runner, is_executable=is_executable)
    return floor_ok(
        ran, enum.total_expected, per_binary_ran, enum.per_binary_expected,
        usable=enum.usable, errors=enum.errors)


# ---------------------------------------------------------------------------
# CLI — gate.sh calls `python3 -m gate.coverage check ...` (Wave 2). The EXIT
# CODE is the decision (0 ok / 3 retry — gate.sh's exit-3 retry contract, same
# as the old inline block). stdout carries the human banner (echoed verbatim
# into the gate log) followed by `@cov:` machine lines the orchestrator
# harvests for telemetry (expected / floor_ok / the per-binary-expected map).
# ---------------------------------------------------------------------------

def _parse_per_binary_ran(s):
    """Parse a "name:count name:count" map (the GATE_BIN_RAN format)."""
    out = {}
    for tok in (s or "").split():
        if ":" in tok:
            k, v = tok.split(":", 1)
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def _main_check(argv):
    """`check --ran N --manifest PATH [--per-binary-ran MAP]` → exit 0|3."""
    p = argparse.ArgumentParser(prog="coverage.py check")
    p.add_argument("--ran", type=int, required=True)
    p.add_argument("--manifest", required=True)
    p.add_argument("--per-binary-ran", default="",
                   help="the ran map in GATE_BIN_RAN form 'name:count ...'")
    args = p.parse_args(argv)
    pbr = _parse_per_binary_ran(args.per_binary_ran)
    v = check_coverage_floor(args.ran, pbr, args.manifest)
    if v.message:
        print(v.message)
    # Machine lines (gate.sh strips these from the echoed banner). Group names
    # are CMake identifiers (alnum + _) and counts are ints, so the space-
    # separated name:count map needs no quoting for the awk merge downstream.
    print("@cov:exit=%d" % v.exit_code)
    print("@cov:expected=%d" % v.expected)
    print("@cov:floor_ok=%d" % (1 if v.ok else 0))
    print("@cov:bin_expected=%s" % " ".join(
        "%s:%d" % (k, c) for k, c in sorted(v.per_binary_expected.items())))
    return v.exit_code


def main(argv):
    if len(argv) >= 2 and argv[1] == "check":
        return _main_check(argv[2:])
    sys.stderr.write("usage: coverage.py check --ran N --manifest PATH\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
