"""fix #3923 — scripts/agent_check.sh silently treated a stray `--`-prefixed
positional as a gtest FILTER SUBSTRING instead of refusing it, and (the root
cause) a `--no-lit` typed BEFORE `--area` never reached the code that
recognized it, so `--area` and its group-name argument fell through
unrecognized right alongside it.

MEASURED REPRO (issue #3923, a real lane hit this in R14): running

    agent_check.sh --no-lit --area codegen f1 f2 f3

silently ran nearly the WHOLE codegen test binary (every suite whose name
contains the literal substring "codegen") instead of the 3 intended named
filters, with `--area` itself ALSO becoming an inert filter substring that
matched nothing. The `--area` dispatch block only ever inspects `$1`; the old
`--no-lit` stripping loop lived INSIDE the default filter-run path, which is
reached only AFTER that `--area` check has already run (and, with `--no-lit`
first, already been skipped) — so `--no-lit` before `--area` desynchronized
the two.

THE FIX moves the `--no-lit` stripping (still "anywhere in the args", not
just a prefix — a bare `--no-lit` after a filter must keep validating, per
the script's own long-standing comment) to BEFORE `--area`/`--build-only`/
`--select` are dispatched, so the two compose in either order, and adds a
generic guard: any argument reaching the plain-filter loop that still starts
with `--` is refused as a likely mistyped/misplaced flag (a gtest
suite-or-name substring is never spelled that way) rather than silently run
as a filter.

WIDENED BY #3934 (review of this fix): the guard above only matched `--*`,
so a SINGLE-dash typo — `-area codegen filter1`, `filter1 -area codegen`,
`--area codegen -no-lit filter1` — leaked through exactly the same way,
one dash short. Since `-`/`--` are not valid C++ identifier characters, a
gtest suite-or-name substring (always a C++ identifier) can NEVER start
with a dash at all — confirmed empirically, not just by grammar: enumerating
build/tests/bezel_tests_codegen (14167 suite.name strings) finds ZERO
containing a hyphen anywhere, let alone leading one. The guard is widened
from `--*` to `-*`, which is a strict superset (every `--foo` is also
`-*`), so this is zero-risk to any legitimate filter and does not touch the
`--gtest_filter*`/`run_parallel*`/`--full`/`--all` cases, which still match
first and keep their tailored messages. `--probe [--asan] FILE.bzl` is
unaffected structurally: that mode dispatches and `exit`s in its own block,
at the very top of the script, before ANY of the code this module extracts
ever runs — its own single/double-dash arguments (`--asan`, and whatever the
caller appends for bezelc) never reach this guard.

HOW THE PIN WORKS (same technique as test_gate_full_cycle_pins.py's
functional pins: extract a block VERBATIM by sentinel from the real script,
source it under bash with `die`/`build_tests`/`test_binaries`/
`binaries_matching` stubbed out, feed it a candidate argv via `set --`, and
read back $AREA/$NO_LIT/$* — or the stubbed `die` call — afterward). This
runs zero cmake/dev_heavy/bezelc — it is pure argument-parsing over the
script's own bytes, so it belongs in the gate-selftest phase.

FAIL-WITHOUT MEASURED (not merely argued): extracting the equivalent region
from the file as it existed at HEAD before this fix (`git show HEAD:` prior
to this commit) and running it through this same harness against
`--no-lit --area codegen f1 f2 f3` yields AREA=[] (never set) and
ARGS=[--area codegen f1 f2 f3] (both `--area` and its group name fell through
as filters, joining the 3 real ones) — reproducing the issue's own
description exactly. The fixed block yields AREA=[codegen] and ARGS=[f1 f2
f3]. `test_no_lit_before_area_still_sets_area_and_keeps_only_real_filters`
below pins the fixed shape; it fails against the pre-fix bytes (confirmed by
hand, not merely asserted here — see the fix commit body for the transcript).

Stdlib only (subprocess/shlex/tempfile/unittest) — no build. Runs inside the
#3415 gate-selftest phase (`python3 -m unittest discover -s scripts/gate/tests`).
This module is about scripts/agent_check.sh only; it never spawns the
top-level push/serialization entry point, so it is out of scope for
test_gate_markers.SelfTestSuiteIsolationTest.KNOWN.
"""

import pathlib
import shlex
import subprocess
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = HERE.parent.parent
AGENT_CHECK_SH = SCRIPTS / "agent_check.sh"

_START = "# --- --no-lit: strip it from ANYWHERE in the remaining args"
_END = "\n\nbuild_tests\n"


def _slice(text, start, end, what):
    """text[start .. end] by sentinel; a miss is a loud failure, never an
    empty block (mirrors test_gate_full_cycle_pins.py's helper of the same
    name/contract — duplicated here rather than imported so this module
    stays a single self-contained file with no cross-test-module coupling)."""
    i = text.find(start)
    if i < 0 or text.find(start, i + 1) >= 0:
        raise AssertionError("%s: start sentinel %r not found exactly once" % (what, start))
    j = text.find(end, i + len(start))
    if j < 0:
        raise AssertionError("%s: end sentinel %r not found after start" % (what, end))
    return text[i:j + len(end)]


# Extracted ONCE at import time so every test in this module runs against the
# identical bytes; a sentinel miss (e.g. the comment wording changed) fails
# EVERY test loudly rather than one test silently matching nothing.
_BLOCK = _slice(AGENT_CHECK_SH.read_text(), _START, _END,
                "agent_check.sh --no-lit/--area/footgun block")

_HARNESS = r'''
set -uo pipefail
die() { echo "DIED: $*" >&2; exit 42; }
build_tests() { echo "STUB build_tests: AREA=[$AREA]" >&2; return 0; }
test_binaries() { :; }
binaries_matching() { :; }
AREA=""
%(setargs)s
%(block)s
echo "RESULT AREA=[$AREA] NO_LIT=[$NO_LIT] ARGS=[$*]"
'''


def _run(argv, timeout=20):
    setargs = "set -- " + " ".join(shlex.quote(a) for a in argv)
    script = _HARNESS % {"setargs": setargs, "block": _BLOCK}
    return subprocess.run(["bash", "-c", script], capture_output=True,
                          text=True, timeout=timeout)


def _result(argv):
    """Run argv through the real block; parse the RESULT line into
    (area, no_lit, args_list). Fails the calling test with full output if the
    block died instead of reaching RESULT (see `_died` for that path)."""
    r = _run(argv)
    line = next((ln for ln in r.stdout.splitlines() if ln.startswith("RESULT ")), None)
    assert line is not None, (
        "block did not reach RESULT for argv=%r (rc=%s)\nstdout:\n%s\nstderr:\n%s"
        % (argv, r.returncode, r.stdout, r.stderr))
    area = line.split("AREA=[", 1)[1].split("]", 1)[0]
    no_lit = line.split("NO_LIT=[", 1)[1].split("]", 1)[0]
    args = line.split("ARGS=[", 1)[1].rsplit("]", 1)[0]
    return area, no_lit, (args.split(" ") if args else [])


class AgentCheckNoLitAreaOrderingTest(unittest.TestCase):
    """issue #3923: `--no-lit` and `--area <group>` must compose in EITHER
    order, and neither may ever leak into the filter list."""

    def test_area_alone_still_works(self):
        area, no_lit, args = _result(["--area", "codegen", "f1"])
        self.assertEqual((area, no_lit, args), ("codegen", "0", ["f1"]))

    def test_no_lit_after_area_still_works(self):
        """The order the script's usage comment has always documented."""
        area, no_lit, args = _result(["--area", "codegen", "--no-lit", "f1"])
        self.assertEqual((area, no_lit, args), ("codegen", "1", ["f1"]))

    def test_no_lit_before_area_still_sets_area_and_keeps_only_real_filters(self):
        """THE MEASURED BUG (issue #3923's own repro shape, 3 filters): with
        `--no-lit` FIRST, `--area` must still be recognized as the group flag
        and the group name must never join the filter list. Before the fix
        this produced AREA=[] and ARGS=[--area codegen f1 f2 f3] — confirmed
        by hand against the pre-fix bytes (see module docstring)."""
        area, no_lit, args = _result(
            ["--no-lit", "--area", "codegen", "f1", "f2", "f3"])
        self.assertEqual(area, "codegen",
                         "--area must be recognized even with --no-lit first")
        self.assertEqual(no_lit, "1")
        self.assertEqual(args, ["f1", "f2", "f3"],
                         "the filter list must be EXACTLY the 3 real filters "
                         "— neither '--area' nor 'codegen' may leak into it")

    def test_no_lit_alone_after_stripping_leaves_no_filter_and_dies(self):
        r = _run(["--no-lit"])
        self.assertEqual(r.returncode, 42)
        self.assertIn("no test filter given", r.stderr)

    def test_plain_filters_with_no_flags_are_unaffected(self):
        area, no_lit, args = _result(["filter1", "filter2"])
        self.assertEqual((area, no_lit, args), ("", "0", ["filter1", "filter2"]))


class AgentCheckUnrecognizedFlagIsRefusedTest(unittest.TestCase):
    """issue #3923's second half, widened by #3934: an argument reaching the
    filter loop that still starts with a DASH — one or two — must be a hard
    error, not a silently-accepted filter substring. This is the general
    form of the `--area`-fell-through bug: neither `--area`/`--no-lit` nor a
    single-dash typo of either may ever leak into the filter list."""

    def test_a_stray_double_dash_flag_is_refused_not_run_as_a_filter(self):
        r = _run(["--no-such-flag", "filter1"])
        self.assertEqual(r.returncode, 42,
                         "an unrecognized --flag must die, not be treated as "
                         "a filter substring (issue #3923)")
        self.assertIn("mistyped or misplaced flag", r.stderr)
        self.assertNotIn("RESULT", r.stdout,
                         "must not reach the point of running anything")

    def test_area_placed_after_a_filter_is_also_refused(self):
        """`--area` is only ever recognized as the leading token; placed
        after a filter it used to fall through as two MORE filters
        ('--area', 'codegen') — the same class of bug as #3923, now caught
        by the same generic guard."""
        r = _run(["filter1", "--area", "codegen"])
        self.assertEqual(r.returncode, 42)
        self.assertIn("mistyped or misplaced flag", r.stderr)

    def test_single_dash_area_typo_is_refused_not_leaked_as_three_filters(self):
        """#3934's MEASURED repro: `-area` (one dash) does not match the
        literal string `--area`, so the dedicated --area block never
        recognizes it either — it must be REFUSED by the generic guard, not
        silently run as a filter (nor, before this widening, as THREE
        filters: '-area', 'codegen', and the real 'filter1')."""
        r = _run(["-area", "codegen", "filter1"])
        self.assertEqual(r.returncode, 42,
                         "a single-dash '-area' must be refused, not treated "
                         "as 1-3 filter substrings (#3934)")
        self.assertIn("mistyped or misplaced flag", r.stderr)
        self.assertNotIn("RESULT", r.stdout)

    def test_single_dash_area_after_a_filter_is_also_refused(self):
        """#3934's second measured shape: the leak is independent of where
        the single-dash typo sits in the argv."""
        r = _run(["filter1", "-area", "codegen"])
        self.assertEqual(r.returncode, 42)
        self.assertIn("mistyped or misplaced flag", r.stderr)

    def test_single_dash_no_lit_typo_is_refused_not_leaked_as_a_filter(self):
        """#3934's third measured shape: `-no-lit` (one dash) is not the
        literal string `--no-lit`, so the no-lit stripping loop leaves it
        alone; it must reach the generic guard and die, not join the filter
        list alongside the real `--area`/filter that surround it."""
        r = _run(["--area", "codegen", "-no-lit", "filter1"])
        self.assertEqual(r.returncode, 42,
                         "a single-dash '-no-lit' must be refused, not "
                         "silently added to the filter list (#3934)")
        self.assertIn("mistyped or misplaced flag", r.stderr)

    def test_existing_refusals_are_unaffected(self):
        for argv, needle in (
            (["--gtest_filter=foo"], "the test binary IGNORES"),
            (["--full"], "is a full-suite run"),
            (["run_parallel_x"], "is a full-suite run"),
            ([""], "empty filter would run the WHOLE suite"),
        ):
            with self.subTest(argv=argv):
                r = _run(argv)
                self.assertEqual(r.returncode, 42, argv)
                self.assertIn(needle, r.stderr, argv)


if __name__ == "__main__":
    unittest.main()
