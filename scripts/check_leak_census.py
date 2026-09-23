#!/usr/bin/env python3
"""check_leak_census.py - the docket for generated programs that LEAK (#4638).

WHY THIS EXISTS
---------------
`runAot` runs every program with `ASAN_OPTIONS=detect_leaks=0` (#1822), so a
generated program could leak and its row stayed green even under `gate.sh
--full`. Measured 2026-09-23 on the #4619 membership-boxing program: 12,000 B
in 500 allocations, reported by a direct `bezelc --sanitize=address` run and by
`runAotChecked`, and invisible to `runAot` under forced ASan.

Flipping the default on is NOT the fix - §28.2 checked-disposal quarantines
disposed objects on purpose, negative tests exit with heap live by design, and
LSan's `exitcode` supersedes the program's own (#4532). So the harness grew an
opt-in third oracle, `BEZEL_AOT_DETECT_LEAKS` (tests/support/force_sanitize.hpp):

    off              byte-for-byte today's behaviour (the default).
    report / census  LSan on; each leak is counted and printed as a `[ LEAK ]`
                     marker on stdout; the row's verdict is UNCHANGED.
    1 / strict / on  LSan on; a leak FAILS the row.

`report` is verdict-preserving by construction, so it can be switched on over
the whole suite without turning a single row red. That is what this script
consumes: it attributes each `[ LEAK ]` marker to the row whose `[ RUN  ]`
bracket encloses it - tests/test_harness.hpp prints
`[ RUN  ] <suite>.<name>` and `[  OK  ]` on the same stream, so position is
exact attribution with no change to the harness's row bookkeeping.

THE DOCKET IS FROZEN, NOT SUPPRESSED
------------------------------------
The leaks this surfaces were always there; nobody could see them. Relaxing an
assertion to hide them would be the move CLAUDE.md forbids. Instead the set of
leaking rows is banked in `scripts/leak_census.baseline` and compared exactly,
the same posture `check_toolchain_skip_surface.py` takes to its 2,046-site
surface and `tests/conformance/rules/baseline.tsv` takes to spec divergences.

  * a row that leaks and is NOT in the baseline  -> FAIL (a new leak)
  * a baselined row that no longer leaks         -> FAIL as STALE HEADROOM;
    bank it with --update. Unbanked headroom is indistinguishable from a rise
    somebody already spent - the rule every sibling ratchet in scripts/ states.

The byte/allocation columns are recorded for triage and are NOT compared: they
are informative (they tell a reader whether a row leaks 24 B or 24 MB) but a
size that drifts must not fail a cycle.

HOW TO PRODUCE A LOG
--------------------
    BEZEL_AOT_DETECT_LEAKS=report \\
      scripts/agent_check.sh --area codegen <filter> 2>&1 | tee run.log
    scripts/check_leak_census.py --from-log run.log --list
    scripts/check_leak_census.py --from-log run.log --update     # bank
    scripts/check_leak_census.py --from-log run.log --strict     # ratchet

Usage:  scripts/check_leak_census.py [--from-log FILE ...] [--strict]
                                     [--update] [--list] [--self-test]
"""
import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / "scripts" / "leak_census.baseline"

# tests/test_harness.hpp: `[ RUN  ] <suite>.<name>` / `[  OK  ]` / `[ FAIL ]`.
RUN_RE = re.compile(r"^\[ RUN  \] (\S+)\s*$")
END_RE = re.compile(r"^\[(?:  OK  | FAIL )\] (\S+)\s*$")
# tests/support/aot_support.hpp noteLeakDetection().
LEAK_RE = re.compile(
    r"^\s*\[ LEAK \] (\S+?): (?:(\d+) byte\(s\) leaked in (\d+) allocation\(s\)"
    r"|LeakSanitizer reported a leak)")


def parse_log(text):
    """-> {row: (bytes, allocs)}; bytes/allocs are None when unsized.

    A `[ LEAK ]` outside any `[ RUN  ]` bracket is attributed to the sentinel
    row `<unattributed>` rather than dropped: silently losing an observation is
    the failure mode this whole issue is about.
    """
    current = None
    out = {}
    for line in text.splitlines():
        m = RUN_RE.match(line)
        if m:
            current = m.group(1)
            continue
        if END_RE.match(line):
            current = None
            continue
        m = LEAK_RE.match(line)
        if m:
            row = current or "<unattributed>"
            b = int(m.group(2)) if m.group(2) else None
            a = int(m.group(3)) if m.group(3) else None
            prev = out.get(row)
            # One row may call runAot several times; keep the largest so the
            # triage column reflects the worst leak that row produces.
            if prev is None or (b or 0) > (prev[0] or 0):
                out[row] = (b, a)
    return out


def read_baseline(path=BASELINE):
    rows = {}
    if not path.exists():
        return rows
    for line in path.read_text().splitlines():
        line = line.rstrip("\n")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        name = parts[0]
        b = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else None
        a = int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else None
        rows[name] = (b, a)
    return rows


HEADER = """\
# leak_census.baseline - generated programs whose LEAK is known and frozen
# (#4638).  One row per `<suite>.<name>`; columns are TAB-separated:
#
#     <suite>.<name>  <bytes>  <allocations>
#
# The SET of rows is compared exactly by scripts/check_leak_census.py --strict;
# the two numeric columns are triage only and are never compared.  A row that
# stops leaking must be REMOVED with --update - unbanked headroom reads exactly
# like a rise somebody already spent.
#
# These leaks were ALWAYS present.  Before #4638 nothing in the suite could
# observe them, so an empty file here does not mean a leak-free tree: it means
# nobody has run the census over that part of it yet.  `coverage:` lines below
# record which filters HAVE been censused, so a reader can tell "clean" from
# "unmeasured".
#
# The `leak_detection_4638.*` rows are the HARNESS'S OWN known-positive
# fixture - the #4619 membership-boxing program, which leaks 24 B per call by
# design and with the reasons recorded.  They are banked rather than excluded
# so that nothing about this instrument is invisible; they are the rows that
# prove the oracle can fire, and they must NOT be "fixed".
#
# HOW BIG IS THE UNMEASURED PART?  Measured 2026-09-23 at f644098f, outside the
# harness, by extracting the 11,030 distinct Bezel programs embedded in
# tests/codegen + tests/ownership and running a 1,200-program random sample
# (seed 4638) through `bezelc --sanitize=address` with LSan on:
#     923 clean / 55 LEAK / 221 rejected-at-compile / 1 AddressSanitizer error
#     => 5.6% of the programs that compile and run LEAK (55 across 46 files).
# The leaking files cluster on boxing and capture-cell lowering, and include
# several whose whole subject is the property they leak on
# (generic_record_string_field_leak_3945, class_field_record_string_release_4253,
# array_string_literal_local_leak_2774, capture_cell_holder_validity_3484).
"""


def write_baseline(rows, coverage, path=BASELINE):
    lines = [HEADER]
    for c in coverage:
        lines.append("# coverage: %s\n" % c)
    lines.append("#\n")
    for name in sorted(rows):
        b, a = rows[name]
        lines.append("%s\t%s\t%s\n" % (name, "" if b is None else b,
                                       "" if a is None else a))
    path.write_text("".join(lines))


def read_coverage(path=BASELINE):
    if not path.exists():
        return []
    return [l.split("# coverage:", 1)[1].strip()
            for l in path.read_text().splitlines() if l.startswith("# coverage:")]


SELF_TEST_LOG = """\
[ RUN  ] alpha.clean_row
[  OK  ] alpha.clean_row
[ RUN  ] alpha.leaky_row
    [ LEAK ] runAot: 12000 byte(s) leaked in 500 allocation(s) - BEZEL_AOT_DETECT_LEAKS=report, row NOT failed (#4638)
[  OK  ] alpha.leaky_row
[ RUN  ] beta.two_calls
    [ LEAK ] runAot: 24 byte(s) leaked in 1 allocation(s) - x
    [ LEAK ] runAot: 9600 byte(s) leaked in 400 allocation(s) - x
[ FAIL ] beta.two_calls
    [ LEAK ] runAot: LeakSanitizer reported a leak (no SUMMARY line) - x
"""


def self_test():
    got = parse_log(SELF_TEST_LOG)
    expect = {
        "alpha.leaky_row": (12000, 500),
        "beta.two_calls": (9600, 400),      # the larger of the row's two calls
        "<unattributed>": (None, None),     # outside any RUN bracket, not dropped
    }
    assert got == expect, "parse_log: %r != %r" % (got, expect)
    assert "alpha.clean_row" not in got, "a clean row must not be banked"

    import tempfile
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "b.baseline"
        write_baseline({"a.b": (1, 2), "c.d": (None, None)}, ["--area x f"], p)
        back = read_baseline(p)
        assert back == {"a.b": (1, 2), "c.d": (None, None)}, back
        assert read_coverage(p) == ["--area x f"], read_coverage(p)

    # The ratchet's two verdicts.
    base = {"a.b": (1, 1)}
    assert sorted(set({"a.b": 0, "new.row": 0}) - set(base)) == ["new.row"]
    assert sorted(set(base) - set({})) == ["a.b"]
    print("check_leak_census.py: self-test OK")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from-log", action="append", default=[],
                    help="a test-run log to read (repeatable); '-' is stdin")
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--coverage", action="append", default=[],
                    help="what this census run covered; recorded with --update")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    base = read_baseline()

    if args.list and not args.from_log:
        cov = read_coverage()
        print("leak docket: %d row(s) banked" % len(base))
        for c in cov:
            print("  coverage: %s" % c)
        total = sum(b for b, _ in base.values() if b)
        for name in sorted(base):
            b, a = base[name]
            print("  %-70s %s B / %s alloc" % (name, b, a))
        print("  total: %d byte(s) across banked rows" % total)
        return 0

    if not args.from_log:
        ap.error("--from-log is required (or use --list / --self-test)")

    text = ""
    for f in args.from_log:
        text += sys.stdin.read() if f == "-" else pathlib.Path(f).read_text(
            errors="replace")
    observed = parse_log(text)

    if args.list:
        print("observed %d leaking row(s):" % len(observed))
        for name in sorted(observed):
            b, a = observed[name]
            print("  %-70s %s B / %s alloc" % (name, b, a))

    if args.update:
        merged = dict(base)
        merged.update(observed)
        write_baseline(merged, sorted(set(read_coverage()) | set(args.coverage)))
        print("banked %d row(s) (%d new); baseline now %d"
              % (len(observed), len(set(observed) - set(base)), len(merged)))
        return 0

    if args.strict:
        # Only rows the log could have observed are eligible for the
        # stale-headroom verdict: a partial census (one --area, one filter)
        # sees a subset of the suite, and failing on rows it never ran would
        # make the ratchet unusable outside a full run.
        ran = set(re.findall(r"^\[ RUN  \] (\S+)\s*$", text, re.M))
        new = sorted(set(observed) - set(base))
        gone = sorted((set(base) & ran) - set(observed))
        rc = 0
        if new:
            print("FAIL: %d row(s) leak and are not in the docket:" % len(new))
            for n in new:
                b, a = observed[n]
                print("  + %-68s %s B / %s alloc" % (n, b, a))
            print("These leaks are REAL. Fix them, or bank them deliberately "
                  "with --update and say why in the commit body.")
            rc = 1
        if gone:
            print("FAIL: %d banked row(s) no longer leak (STALE HEADROOM):"
                  % len(gone))
            for n in gone:
                print("  - %s" % n)
            print("Bank the win with --update.")
            rc = 1
        if rc == 0:
            print("check_leak_census.py: OK (%d observed, %d banked, %d rows ran)"
                  % (len(observed), len(base), len(ran)))
        return rc

    return 0


if __name__ == "__main__":
    sys.exit(main())
