"""#4533 (+ #4537) — the sharded sanitizer-report hard-fail must be able to
FIRE, and it must fire on the right thing.

#4537 closes the residual gap #4533 deliberately left open: a LEAK report
(`ERROR: LeakSanitizer`) in a test that PASSED was counted and printed but
never fatal, because 37 rows across 5 distinct assertion shapes deliberately
assert one (a naive grep for `leaksAtLeast` alone finds only 11 of them — see
scripts/gate/sanitizer_leak_exempt.txt for the other four). Since #4533's
`[san-scope]` markers already attribute every report to its test, the fix is
the identical rule corruption already gets — fatal in a failing test, fatal in
a passing test unless declared, allowed when declared — against a SEPARATE
list (scripts/gate/sanitizer_leak_exempt.txt) because the rationale is
different: a leak row is a deliberate ASSERTION, not a UB allowance.

THE DEFECT. `scripts/gate.sh` treats a fired generated-code sanitizer report as
DETERMINISTIC evidence of a memory error and hard-fails on it, independent of
the REAL-vs-FLAKY arbitration. It looked for one by grepping `$S/gate_shard.log`
— `tests/run_parallel.sh`'s OWN STDOUT. A shard's PROGRAM output never reaches
that stream, so the grep was over a file that STRUCTURALLY CANNOT hold the
pattern, and a grep that returns 0 for that reason is indistinguishable from a
clean suite.

MEASURED, on the failed 2026-09-21 `--full` cycle at e89577e4:

    grep -cE 'ERROR: (Address|Leak)Sanitizer' e89577e4_full.log          -> 0
    same grep over that run's kept per-shard logs   -> 306 lines, 12 of 48 logs
    grep -cE 'ERROR: AddressSanitizer' (corruption, not leaks)           -> 7
      (4 double-free in bz_closure_release from bz$async$M.C$Fill$Resume —
       the open #3460 race; 3 SEGV)

and the cycle recorded `shard_asan  0.01s  rc=0 (no ASan report)`.

WHY ATTRIBUTION IS PART OF THE FIX. Lifting the headlines is not enough,
because a report is NOT uniformly a defect: ~11 rows assert a leak deliberately
(`leaksAtLeast`), and rows that exercise §28.2 use-after-dispose in UNCHECKED
mode run a program the spec leaves UNDEFINED — ASan reports it and the row
passes. So the gate must know WHICH TEST produced a report, and that cannot be
read off the `[ RUN  ]` lines: those go to block-buffered `std::cout` while the
report goes to unbuffered fd 2. An attempt to attribute the real reports that
way named a plausible, wrong test. `tests/test_main.cpp` therefore brackets each
body with `[san-scope]` markers ON fd 2 under the sweep, which is the only
stream ordering that means anything.

WHAT THESE TESTS PIN, end to end:
  1. ScannerTest / VerdictTest — the decision, over every shape, in isolation.
  2. LiftTest — the REAL tests/run_parallel.sh with a stub binary that honours
     the shard contract: the evidence survives a CLEAN run (where the logdir is
     reaped) as well as a failing one (where it is kept).
  3. GateBlockTest / CanaryBlockTest — the REAL gate.sh blocks, including the
     pre-fix state (no accounting line) which must now FAIL.
  4. MarkerContractTest — test_main.cpp's markers: fd 2, sweep-gated, and a
     spelling that cannot collide with summary.py's `^[ RUN` culprit grep.
  5. DeliberateReportCaptureTest / TautologyGuardTest — the two hazards this
     change could have created and does not.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCRIPTS = HERE.parent.parent          # scripts/
REPO = SCRIPTS.parent
GATE_SH = SCRIPTS / "gate.sh"
CYCLE_SH = SCRIPTS / "gate_full_cycle.sh"
RP_SH = REPO / "tests" / "run_parallel.sh"
TEST_MAIN = REPO / "tests" / "test_main.cpp"
DRS_TEST = REPO / "tests" / "support" / "driver_run_support_test.cpp"

sys.path.insert(0, str(SCRIPTS))
from gate import sanitizer as S      # noqa: E402
from gate import summary as _summary  # noqa: E402

# A REAL clang/ASan report, captured verbatim from an instrumented program
# (`int *p = malloc(16); free(p); p[1] = 7;` built with -fsanitize=address),
# wrapped in the banner aot_support.hpp's emitSanitizerReportToStderr writes to
# fd 2 (#1822). Recorded rather than generated so the fixture is deterministic
# and needs no toolchain; LiftTest exercises the real path end to end.
CORRUPTION = """
===== [aot-asan #1822] sanitizer fired in an AOT test program =====
=================================================================
==3871325==ERROR: AddressSanitizer: heap-use-after-free on address 0x7b68c07e0014 at pc 0x4e9e9f bp 0x7fffe805cfb0 sp 0x7fffe805cfa8
WRITE of size 4 at 0x7b68c07e0014 thread T0
    #0 0x4e9e9e in main uaf.c:2:66
SUMMARY: AddressSanitizer: heap-use-after-free uaf.c:2:66 in main
===== [aot-asan #1822] end sanitizer report =====
"""

# A leak report's headline is `ERROR: LeakSanitizer`; its only AddressSanitizer
# mention is the `SUMMARY:` line. 11 rows in 7 files re-emit exactly this shape
# while PASSING, so it must never read as corruption.
LEAK = """
===== [aot-asan #1822] sanitizer fired in an AOT test program =====
=================================================================
==3414155==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 8248 byte(s) in 103 object(s) allocated from:
    #0 0x4c1d3d in malloc
SUMMARY: AddressSanitizer: 8248 byte(s) leaked in 103 allocation(s).
===== [aot-asan #1822] end sanitizer report =====
"""

STUB = r"""#!/usr/bin/env bash
set -u
c="${BEZEL_SHARD_COUNT:-1}"; i="${BEZEL_SHARD_INDEX:-0}"
total=2; passed=0; failed=0
for (( t = 0; t < total; t++ )); do
  (( t % c == i )) || continue
  echo "[ RUN  ] stub.t$t"
  # The same bracket test_main.cpp writes, on the same unbuffered stream.
  echo "[san-scope] enter stub.t$t" >&2
  if [[ "$t" -eq 1 && -n "${STUB_REPORT:-}" ]]; then cat "$STUB_REPORT" >&2; fi
  if [[ "$t" -eq 1 && -n "${STUB_FAIL:-}" ]]; then
    echo "[san-scope] leave stub.t$t fail" >&2
    failed=$(( failed + 1 )); echo "[ FAIL ] stub.t$t"
  else
    echo "[san-scope] leave stub.t$t pass" >&2
    passed=$(( passed + 1 )); echo "[  OK  ] stub.t$t"
  fi
done
if [[ -n "${BEZEL_SHARD_SUMMARY:-}" ]]; then
  printf '{"schema":"bezel.shard-summary.v1","passed":%d,"failed":%d,"total":%d,"selected":%d,"native_sanitizer_unavailable_skips":0,"checked_toolchain_skips":0}\n' \
    "$passed" "$failed" "$total" "$(( passed + failed ))" > "$BEZEL_SHARD_SUMMARY"
fi
echo
echo "$passed passed, $failed failed (shard $i/$c), $total total"
exit 0
"""

GATE_HARNESS = r"""
set -u
now() { echo 0; }
rec_ran() { echo "REC_RAN $1 $2 ${3:-}"; }
fail() { FAILED_PHASE="$1"; echo "GATE_FAIL: $1"; exit 1; }
S="$PIN_TMP"
ASAN=1
SHARD_COUNT=7
ASAN_ERRS=0; ASAN_SHARDS=0
PHASE_T0=0
@@HELPER@@
@@BLOCK@@
echo "AFTER_ASAN_BLOCK ASAN_ERRS=$ASAN_ERRS ASAN_SHARDS=$ASAN_SHARDS"
"""

CANARY_HARNESS = r"""
set -u
fail() { echo "GATE_FAIL: $1"; exit 1; }
_canary_dual() { echo "CANARY_DUAL $1"; }
S="$PIN_TMP"
ASAN=1
_c_asan=0
@@HELPER@@
@@BLOCK@@
echo "AFTER_CANARY_BLOCK _c_asan=$_c_asan"
"""

_HELPER_START = "gate_sanitizer_verdict() { # $1 = sharded-run log"
_HELPER_END = "\n}\n"
_ASAN_BLOCK_START = ('if [ "$ASAN" -eq 1 ]; then\n'
                     '  PHASE_T0=$(now)  # shard_asan is only the harvest')
_ASAN_BLOCK_END = ('    *) rec_ran shard_asan "1" "evidence reader exited $asan_rc"\n'
                   '       fail "sharded ASan — the sanitizer-evidence reader exited $asan_rc; its silence is not a clean suite (#4533)" ;;\n'
                   '  esac\nfi\n')
_CANARY_BLOCK_START = ('      if [ "$ASAN" -eq 1 ]; then\n'
                       '        _c_find=$(gate_sanitizer_verdict')
_CANARY_BLOCK_END = '        fi\n      fi\n'


def _slice(text, start, end, what):
    idx = text.find(start)
    if idx < 0 or text.find(start, idx + 1) >= 0:
        raise AssertionError(
            "%s: start sentinel %r not found exactly once" % (what, start))
    e = text.find(end, idx + len(start))
    if e < 0:
        raise AssertionError("%s: end sentinel not found after start" % what)
    return text[idx:e + len(end)]


def _logdir(tmp, **logs):
    d = pathlib.Path(tmp) / "logs"
    d.mkdir(exist_ok=True)
    for name, body in logs.items():
        (d / ("shard.%s.log" % name)).write_text(body)
    return str(d)


def _scope(name, outcome, body=""):
    return ("[san-scope] enter %s\n%s[san-scope] leave %s %s\n"
            % (name, body, name, outcome))


class ScannerTest(unittest.TestCase):
    """Attribution: a report belongs to the scope that encloses it."""

    def test_report_in_a_passing_scope_is_attributed_to_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            sc = S.scan_logdir(_logdir(tmp, **{"a.0": _scope("s.p", "pass", CORRUPTION)}))
            self.assertEqual(sc.scanned, 1)
            self.assertEqual(sc.files, 1)
            self.assertEqual(sc.corrupt, 1)
            self.assertEqual(sc.attributed(), (0, 1, 0))
            self.assertEqual(sc.by_test["s.p"]["outcome"], "pass")

    def test_report_in_a_failing_scope_is_attributed_to_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            sc = S.scan_logdir(_logdir(tmp, **{"a.0": _scope("s.f", "fail", CORRUPTION)}))
            self.assertEqual(sc.attributed(), (1, 0, 0))

    def test_a_report_outside_every_scope_is_unattributed_not_dropped(self):
        """The markers are off (plain oracle), or a worker thread reported
        after the body returned. Fail-closed needs the count, not silence."""
        with tempfile.TemporaryDirectory() as tmp:
            sc = S.scan_logdir(_logdir(tmp, **{"a.0": CORRUPTION}))
            self.assertEqual(sc.corrupt, 1)
            self.assertEqual(sc.attributed(), (0, 0, 1))
            self.assertEqual(sc.by_test, {})

    def test_an_unclosed_scope_keeps_its_evidence(self):
        """A shard that died mid-test never writes `leave`. Its report must
        become unattributed — which is FATAL — not vanish."""
        with tempfile.TemporaryDirectory() as tmp:
            body = ("[san-scope] enter s.crashed\n" + CORRUPTION)
            sc = S.scan_logdir(_logdir(tmp, **{"a.0": body}))
            self.assertEqual(sc.attributed(), (0, 0, 1))

    def test_a_leak_is_never_counted_as_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            sc = S.scan_logdir(_logdir(tmp, **{"a.0": _scope("s.leak", "pass", LEAK)}))
            self.assertGreater(sc.hits, 0)
            self.assertEqual(
                sc.corrupt, 0,
                "`SUMMARY: AddressSanitizer: N byte(s) leaked` must not read "
                "as an `ERROR: AddressSanitizer` corruption headline")

    def test_the_scanned_count_travels_with_the_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            sc = S.scan_logdir(_logdir(tmp, **{"a.0": "nothing\n", "a.1": "here\n"}))
            line = S.format_scan(sc)[0]
            self.assertIn("0 report marker(s) in 0 of 2 shard log(s) scanned", line)

    def test_corruption_logs_lead_the_bounded_excerpt(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = _logdir(tmp, **{
                "a.0": _scope("s.l0", "pass", LEAK),
                "a.1": _scope("s.l1", "pass", LEAK),
                "a.2": _scope("s.l2", "pass", LEAK),
                "z.9": _scope("s.c", "fail", CORRUPTION)})
            out = "\n".join(S.format_scan(S.scan_logdir(d)))
            section = out[out.index(S.EXCERPT_HEADER):]
            self.assertIn("==> shard.z.9.log <==", section.splitlines()[1],
                          "the corruption-bearing log must be shown FIRST:\n" + section)
            self.assertIn("heap-use-after-free", section)

    def test_every_corruption_line_lands_in_exactly_one_bucket(self):
        """The invariant the verdict rests on: failing + passing +
        unattributed == corrupt. If a line could fall between the buckets it
        would be silently excused."""
        with tempfile.TemporaryDirectory() as tmp:
            d = _logdir(tmp, **{
                "a.0": _scope("s.p", "pass", CORRUPTION) + CORRUPTION,
                "a.1": _scope("s.f", "fail", CORRUPTION + LEAK),
                "a.2": "[san-scope] enter s.crashed\n" + CORRUPTION,
                "a.3": _scope("s.clean", "pass", LEAK)})
            sc = S.scan_logdir(d)
            self.assertEqual(sum(sc.attributed()), sc.corrupt)
            self.assertEqual(sc.attributed(), (1, 1, 2))

    def test_the_same_test_failing_anywhere_reports_as_failing(self):
        """Round-robin sharding can run a name in one shard and a same-named
        row in another; a FATAL verdict must not be dodged by the passing one."""
        with tempfile.TemporaryDirectory() as tmp:
            d = _logdir(tmp, **{"a.0": _scope("s.x", "pass", CORRUPTION),
                                "a.1": _scope("s.x", "fail", CORRUPTION)})
            sc = S.scan_logdir(d)
            self.assertEqual(sc.by_test["s.x"]["outcome"], "fail")
            self.assertEqual(sc.attributed(), (2, 0, 0))

    # ── fix #4537: leaks get the SAME fail-closed attribution as corruption,
    # tracked in a SEPARATE counter (unattributed_leaks) so the pre-existing
    # `sum(attributed()) == corrupt` invariant above cannot be diluted by a
    # leak line landing in the corruption bucket.

    def test_a_leak_outside_every_scope_is_unattributed_not_dropped(self):
        with tempfile.TemporaryDirectory() as tmp:
            sc = S.scan_logdir(_logdir(tmp, **{"a.0": LEAK}))
            self.assertGreater(sc.hits, 0)
            self.assertEqual(sc.corrupt, 0)
            self.assertEqual(sc.unattributed_leaks, 1)
            self.assertEqual(sc.by_test, {})

    def test_an_unclosed_scope_keeps_its_leak_evidence(self):
        """A shard that died mid-test never writes `leave`. Its leak report
        must become unattributed — which #4537 makes FATAL — not vanish."""
        with tempfile.TemporaryDirectory() as tmp:
            body = ("[san-scope] enter s.crashed\n" + LEAK)
            sc = S.scan_logdir(_logdir(tmp, **{"a.0": body}))
            self.assertEqual(sc.unattributed_leaks, 1)
            self.assertEqual(sc.by_test, {})

    def test_every_leak_line_lands_in_exactly_one_bucket(self):
        """The leak sibling of test_every_corruption_line_lands_in_exactly_one_
        bucket: attributed (via by_test rows) + unattributed_leaks must equal
        every leak line seen, or one could silently evade #4537."""
        with tempfile.TemporaryDirectory() as tmp:
            d = _logdir(tmp, **{
                "a.0": _scope("s.p", "pass", LEAK) + LEAK,
                "a.1": _scope("s.f", "fail", LEAK + CORRUPTION),
                "a.2": "[san-scope] enter s.crashed\n" + LEAK,
                "a.3": _scope("s.clean", "pass", CORRUPTION)})
            sc = S.scan_logdir(d)
            attributed_leaks = sum(r["leak"] for r in sc.by_test.values())
            self.assertEqual(sc.leak, attributed_leaks + sc.unattributed_leaks)
            self.assertEqual((attributed_leaks, sc.unattributed_leaks), (2, 2))


class VerdictTest(unittest.TestCase):
    """The decision, over the shapes the gate must tell apart."""

    def _v(self, scan, exempt=None, leak_exempt=None):
        ev, rows = S.parse_lifted("\n".join(S.format_scan(scan)))
        return S.verdict(ev, rows, exempt or {}, leak_exempt or {})

    def _scan(self, **logs):
        with tempfile.TemporaryDirectory() as tmp:
            return S.scan_logdir(_logdir(tmp, **logs))

    def test_no_accounting_line_is_UNWIRED_not_clean(self):
        """THE #4533 REGRESSION PIN — this is the pre-fix world exactly."""
        rc, lines = S.verdict(*S.parse_lifted("TOTAL: 16482 passed, 0 failed\n"), exempt={})
        self.assertEqual(rc, S.UNWIRED)
        self.assertIn("not wired", " ".join(lines))

    def test_a_scan_that_read_no_shard_log_is_UNWIRED(self):
        rc, lines = self._v(S.Scan())
        self.assertEqual(rc, S.UNWIRED)
        self.assertIn("read 0 shard logs", " ".join(lines))

    def test_clean_is_clean(self):
        rc, lines = self._v(self._scan(**{"a.0": "nothing\n"}))
        self.assertEqual(rc, S.OK)
        self.assertEqual(lines, [])

    def test_corruption_in_a_failing_test_is_FATAL(self):
        rc, lines = self._v(self._scan(**{"a.0": _scope("s.f", "fail", CORRUPTION)}))
        self.assertEqual(rc, S.FATAL)
        self.assertIn("FATAL  s.f", "\n".join(lines))
        self.assertIn("not arbitrable", "\n".join(lines))

    def test_corruption_in_a_failing_test_is_FATAL_even_when_exempt(self):
        """The list is consulted only for tests that PASSED — an exemption is a
        claim about a deliberate outcome, not a licence."""
        rc, _ = self._v(self._scan(**{"a.0": _scope("s.f", "fail", CORRUPTION)}),
                        exempt={"s.f": "deliberate"})
        self.assertEqual(rc, S.FATAL)

    def test_corruption_in_an_undeclared_passing_test_is_FATAL_and_names_it(self):
        rc, lines = self._v(self._scan(**{"a.0": _scope("s.ub", "pass", CORRUPTION)}))
        self.assertEqual(rc, S.FATAL)
        self.assertIn("FATAL  s.ub", "\n".join(lines))
        self.assertIn("sanitizer_exempt.txt", "\n".join(lines),
                      "the cycle must say how to declare a deliberate row")

    def test_corruption_in_a_declared_passing_test_is_allowed_and_says_why(self):
        rc, lines = self._v(self._scan(**{"a.0": _scope("s.ub", "pass", CORRUPTION)}),
                            exempt={"s.ub": "§28.2 unchecked use-after-dispose is UB"})
        self.assertEqual(rc, S.OK)
        self.assertIn("allowed s.ub", "\n".join(lines))
        self.assertIn("§28.2", "\n".join(lines))

    def test_unattributed_corruption_is_FATAL(self):
        rc, lines = self._v(self._scan(**{"a.0": CORRUPTION}))
        self.assertEqual(rc, S.FATAL)
        self.assertIn("could not be attributed", "\n".join(lines))
        self.assertIn("not an excused one", "\n".join(lines))

    # ── fix #4537: a leak is now fatal by the IDENTICAL rule as corruption,
    # against the separate leak_exempt list.

    def test_leak_in_an_undeclared_passing_test_is_FATAL_and_names_it(self):
        rc, lines = self._v(self._scan(**{"a.0": _scope("s.leak", "pass", LEAK)}))
        self.assertEqual(rc, S.FATAL)
        self.assertIn("FATAL  s.leak", "\n".join(lines))
        self.assertIn("sanitizer_leak_exempt.txt", "\n".join(lines),
                      "the cycle must say how to declare a deliberate leak row")
        self.assertIn("#4537", "\n".join(lines))

    def test_leak_in_a_declared_passing_test_is_allowed_and_says_why(self):
        rc, lines = self._v(
            self._scan(**{"a.0": _scope("s.leak", "pass", LEAK)}),
            leak_exempt={"s.leak": "leaksAtLeast — deliberate assertion"})
        self.assertEqual(rc, S.OK)
        self.assertIn("allowed s.leak", "\n".join(lines))
        self.assertIn("leaksAtLeast", "\n".join(lines))

    def test_leak_in_a_failing_test_is_FATAL_even_when_leak_declared(self):
        """Same rule as corruption: the list is consulted only for tests that
        PASSED — a leak declaration is not a licence in a failing test."""
        rc, _ = self._v(
            self._scan(**{"a.0": _scope("s.leak", "fail", LEAK)}),
            leak_exempt={"s.leak": "deliberate"})
        self.assertEqual(rc, S.FATAL)

    def test_a_corruption_exemption_does_not_excuse_a_leak_in_the_same_row(self):
        """The two lists are independent — declaring a row in
        sanitizer_exempt.txt (corruption) says nothing about a leak the same
        row also produced."""
        body = CORRUPTION + LEAK
        rc, lines = self._v(self._scan(**{"a.0": _scope("s.both", "pass", body)}),
                            exempt={"s.both": "the corruption is UB"})
        self.assertEqual(rc, S.FATAL)
        self.assertIn("allowed s.both", "\n".join(lines))   # the corruption half
        self.assertIn("FATAL  s.both", "\n".join(lines))    # the undeclared leak

    def test_unattributed_leak_is_FATAL(self):
        rc, lines = self._v(self._scan(**{"a.0": LEAK}))
        self.assertEqual(rc, S.FATAL)
        self.assertIn("leak report(s) could not be attributed", "\n".join(lines))
        self.assertIn("not an excused one", "\n".join(lines))

    def test_a_missing_exempt_file_makes_the_gate_stricter_never_looser(self):
        self.assertEqual(S.read_exempt("/nonexistent/sanitizer_exempt.txt"), {})
        self.assertEqual(S.read_exempt("/nonexistent/sanitizer_leak_exempt.txt"), {})

    def test_the_shipped_exempt_file_parses_and_every_entry_has_a_reason(self):
        path = SCRIPTS / "gate" / "sanitizer_exempt.txt"
        self.assertTrue(path.exists(), "the exemption file must be tracked")
        for name, reason in S.read_exempt(str(path)).items():
            self.assertTrue(
                reason,
                "%s is exempted with no reason — an undocumented exemption is "
                "indistinguishable from a silenced defect" % name)

    def test_the_shipped_leak_exempt_file_parses_and_every_entry_has_a_reason(self):
        path = SCRIPTS / "gate" / "sanitizer_leak_exempt.txt"
        self.assertTrue(path.exists(), "the leak-exemption file must be tracked")
        for name, reason in S.read_exempt(str(path)).items():
            self.assertTrue(
                reason,
                "%s is exempted with no reason — an undocumented exemption is "
                "indistinguishable from a silenced defect" % name)

    @staticmethod
    def _all_bezel_tests():
        """Every "suite.name" declared anywhere under tests/**/*.cpp via
        BEZEL_TEST(suite, name). Used to catch a STALE leak-exempt entry (row
        deleted or renamed) without assuming any one source pattern."""
        import re
        test_re = re.compile(
            r"BEZEL_TEST\(\s*([A-Za-z0-9_]+)\s*,\s*\n?\s*([A-Za-z0-9_]+)\s*\)")
        found = set()
        for path in (REPO / "tests").rglob("*.cpp"):
            for m in test_re.finditer(path.read_text()):
                found.add("%s.%s" % (m.group(1), m.group(2)))
        return found

    def test_the_shipped_leak_exempt_file_names_only_real_tests(self):
        """Every declared key must resolve to a REAL BEZEL_TEST(suite, name)
        somewhere in tests/ — catches a row that was deleted or renamed out
        from under its declaration, which would otherwise be silent dead
        cover (masking a real future leak in whatever replaced it).

        This does NOT try to re-derive the full population from a single
        source pattern: #4537's own investigation found FIVE distinct
        deliberate-leak-assertion shapes (leaksAtLeast, expectLeakButNo-
        MemoryError, expectLeakNotCrash, an inline equivalent, and
        diagnosticMessageContains(..., "LSan/ASan at exit")) across the 37
        rows this file declares — a single mechanical derivation would not
        generalize the way sanitizer_exempt.txt's UB-corruption population
        does, and pinning it to one pattern here almost shipped an
        undercount (see sanitizer_leak_exempt.txt's RESIDUAL RISK note)."""
        declared = set(S.read_exempt(
            str(SCRIPTS / "gate" / "sanitizer_leak_exempt.txt")))
        self.assertTrue(declared, "guard: the file must not be empty")
        real = self._all_bezel_tests()
        stale = declared - real
        self.assertEqual(
            stale, set(),
            "sanitizer_leak_exempt.txt declares %r, which no longer names a "
            "real BEZEL_TEST(suite, name) — remove the stale entry" % stale)

    def test_the_leaksAtLeast_population_is_a_subset_of_the_declared_leak_exempt(self):
        """The one sub-population that DOES have a single reliable mechanical
        signature: every BEZEL_TEST calling EXPECT_TRUE(leaksAtLeast(...))
        must be declared. A SUBSET check (not equality, unlike the corruption
        file's analogous check) because #4537 found four OTHER deliberate-leak
        shapes that also belong in this file but share no common regex with
        this one."""
        import re
        test_re = re.compile(
            r"BEZEL_TEST\(\s*([A-Za-z0-9_]+)\s*,\s*\n?\s*([A-Za-z0-9_]+)\s*\)")
        derived = set()
        for path in (REPO / "tests").rglob("*.cpp"):
            text = path.read_text()
            if "EXPECT_TRUE(leaksAtLeast" not in text:
                continue
            starts = [(m.start(), m.group(1), m.group(2))
                     for m in test_re.finditer(text)]
            starts.sort()
            for lm in re.finditer(r"EXPECT_TRUE\(leaksAtLeast", text):
                cand = [s for s in starts if s[0] <= lm.start()]
                self.assertTrue(cand, "%s: leaksAtLeast with no enclosing "
                                      "BEZEL_TEST" % path)
                _, suite, name = cand[-1]
                derived.add("%s.%s" % (suite, name))
        declared = set(S.read_exempt(
            str(SCRIPTS / "gate" / "sanitizer_leak_exempt.txt")))
        missing = derived - declared
        self.assertEqual(
            missing, set(),
            "a BEZEL_TEST calling leaksAtLeast(...) is not declared in "
            "sanitizer_leak_exempt.txt: %r" % missing)


def _env(tmp):
    env = dict(os.environ)
    env["TMPDIR"] = tmp
    env["BEZEL_SHARD_LOG_KEEP"] = os.path.join(tmp, "keep")
    for k in ("BEZEL_TEST_FILTER", "BEZEL_TEST_LIST", "BEZEL_RUN_SUMMARY",
              "BEZEL_SHARD_TIMEOUT", "BEZEL_AOT_FORCE_SANITIZE"):
        env.pop(k, None)
    return env


def _run_parallel(tmp, report=None, fail=False, conc=2):
    stub = pathlib.Path(tmp) / "stub_bin.sh"
    stub.write_text(STUB)
    stub.chmod(0o755)
    env = _env(tmp)
    env["STUB_REPORT"] = ""
    if report is not None:
        rp = pathlib.Path(tmp) / "report.txt"
        rp.write_text(report)
        env["STUB_REPORT"] = str(rp)
    env["STUB_FAIL"] = "1" if fail else ""
    return subprocess.run(["bash", str(RP_SH), str(conc), str(stub)],
                          cwd=tmp, env=env, capture_output=True, text=True,
                          timeout=180)


class LiftTest(unittest.TestCase):
    """The producer half, through the REAL runner."""

    def test_clean_run_prints_the_accounting_line_with_a_nonzero_scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = _run_parallel(tmp)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            ev, rows = S.parse_lifted(r.stdout)
            self.assertIsNotNone(ev, "no accounting line:\n" + r.stdout)
            self.assertEqual((ev["hits"], ev["corrupt"], ev["scanned"]), (0, 0, 2))
            self.assertEqual(rows, [])
            self.assertNotIn(S.EXCERPT_HEADER, r.stdout)

    def test_a_report_in_a_PASSING_test_of_a_CLEAN_run_is_lifted_and_attributed(self):
        """#4533's central case: the run exits 0 and every test passes, so
        neither the exit-code path nor the arbitration sees anything — and
        _rp_keep reaps the logdir, so nothing survives for a later reader."""
        with tempfile.TemporaryDirectory() as tmp:
            r = _run_parallel(tmp, report=CORRUPTION)
            self.assertEqual(r.returncode, 0,
                             "guard: the stub run is green — the report is the "
                             "ONLY signal:\n" + r.stdout + r.stderr)
            ev, rows = S.parse_lifted(r.stdout)
            self.assertIsNotNone(ev, r.stdout)
            self.assertEqual((ev["corrupt"], ev["passing"], ev["unattributed"]),
                             (1, 1, 0))
            self.assertEqual([x["test"] for x in rows], ["stub.t1"],
                             "the report must be attributed to the test that "
                             "produced it, by name")
            self.assertIn("heap-use-after-free", r.stdout)
            self.assertFalse(
                any(pathlib.Path(tmp).glob("bezel_shards.*")),
                "guard: a clean run reaps its logdir, so a later grep over it "
                "would find nothing — which is why the lift runs inline")

    def test_a_failing_run_still_lifts_attributes_and_keeps_its_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = _run_parallel(tmp, report=CORRUPTION, fail=True)
            self.assertNotEqual(r.returncode, 0, r.stdout)
            ev, rows = S.parse_lifted(r.stdout)
            self.assertIsNotNone(ev, r.stdout)
            self.assertEqual(ev["failing"], 1)
            self.assertEqual(rows[0]["outcome"], "fail")
            self.assertIn("--- failing tests ---", r.stdout,
                          "guard: the pre-existing FAIL lift is untouched")
            self.assertEqual(len(list((pathlib.Path(tmp) / "keep").glob("*-rc*"))), 1,
                             "the #3010 keep must still happen")

    def test_a_leak_report_is_lifted_without_a_corruption_headline(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = _run_parallel(tmp, report=LEAK)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            ev, _ = S.parse_lifted(r.stdout)
            self.assertGreater(ev["hits"], 0)
            self.assertEqual(ev["corrupt"], 0)

    def test_the_stub_json_is_the_schema_the_real_accounting_reads(self):
        """Guard on the fixture: a stub whose JSON `account` rejects would make
        every row above vacuous."""
        self.assertIn('"schema":"%s"' % _summary.SCHEMA, STUB)


class GateBlockTest(unittest.TestCase):
    """The consumer half: gate.sh's REAL ASan phase."""

    def _run(self, tmp, shard_log):
        (pathlib.Path(tmp) / "gate_shard.log").write_text(shard_log)
        text = GATE_SH.read_text()
        helper = _slice(text, _HELPER_START, _HELPER_END, "gate_sanitizer_verdict")
        block = _slice(text, _ASAN_BLOCK_START, _ASAN_BLOCK_END, "gate.sh ASan phase")
        self.assertIn("gate_sanitizer_verdict", block,
                      "guard: the extracted block never consults the channel")
        env = dict(os.environ)
        env["PIN_TMP"] = tmp
        script = (GATE_HARNESS.replace("@@HELPER@@", helper)
                              .replace("@@BLOCK@@", block))
        # The extracted bash runs `PYTHONPATH=scripts python3 -m gate.sanitizer`
        # relative to cwd, so cwd must be the repo.
        return subprocess.run(["bash", "-c", script], cwd=str(REPO), env=env,
                              capture_output=True, text=True, timeout=60)

    @staticmethod
    def _log(**logs):
        with tempfile.TemporaryDirectory() as tmp:
            return "\n".join(S.format_scan(S.scan_logdir(_logdir(tmp, **logs)))) + "\n"

    def test_a_missing_accounting_line_FAILS_instead_of_reading_as_clean(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, "TOTAL: 16482 passed, 0 failed (across 96 shards)\n")
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("GATE_FAIL:", r.stdout)
            self.assertIn("#4533", r.stdout)
            self.assertIn("REC_RAN shard_asan 1", r.stdout)
            self.assertNotIn("AFTER_ASAN_BLOCK", r.stdout)

    def test_a_clean_scan_passes_and_records_the_scan(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, self._log(**{"a.0": "nothing\n"}))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("REC_RAN shard_asan 0 no fatal finding", r.stdout)
            self.assertIn("1 shard log(s) scanned", r.stdout)
            self.assertIn("AFTER_ASAN_BLOCK", r.stdout)

    def test_corruption_in_a_failing_test_FAILS_the_cycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, self._log(**{"a.0": _scope("s.f", "fail", CORRUPTION)}))
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("GATE_FAIL: sharded ASan — memory error", r.stdout)
            self.assertIn("FATAL  s.f", r.stdout, "the cycle must NAME the test")
            self.assertIn("heap-use-after-free", r.stdout,
                          "the operator must be shown the report")

    def test_corruption_in_an_undeclared_passing_test_FAILS_the_cycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, self._log(**{"a.0": _scope("s.ub", "pass", CORRUPTION)}))
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("FATAL  s.ub", r.stdout)

    def test_an_undeclared_leak_only_run_FAILS_the_cycle(self):
        """fix #4537: the residual gap #4533 left open. gate_sanitizer_verdict
        never passes --leak-exempt, so this run consults the REAL shipped
        sanitizer_leak_exempt.txt — "s.leak" is not in it."""
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, self._log(**{"a.0": _scope("s.leak", "pass", LEAK)}))
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("GATE_FAIL: sharded ASan — memory error", r.stdout)
            self.assertIn("FATAL  s.leak", r.stdout, "the cycle must NAME the test")
            self.assertIn("sanitizer_leak_exempt.txt", r.stdout)
            self.assertNotIn("AFTER_ASAN_BLOCK", r.stdout,
                             "fail() exits before this echo is reached")

    def test_a_declared_leak_only_run_passes_the_cycle(self):
        """The 11 real leaksAtLeast rows are declared in the SHIPPED
        sanitizer_leak_exempt.txt — exercise one end to end through the real
        gate.sh block, not just the unit-level verdict()."""
        declared = ("class_field_record_string_release_4253."
                   "non_sealed_class_still_leaks_axis_sealed")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, self._log(**{"a.0": _scope(declared, "pass", LEAK)}))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("allowed %s" % declared, r.stdout)
            self.assertIn("AFTER_ASAN_BLOCK ASAN_ERRS=2", r.stdout,
                          "the marker count must reach telemetry even for an "
                          "allowed leak — a drift is then visible in the trend")


class CanaryBlockTest(unittest.TestCase):
    """The canary tier launches run_parallel too, so it carried the SAME defect
    — the issue's note that it "reads a direct binary run" does not hold. It
    must read the same channel and stay FAIL-OPEN, since it is never the
    acceptance authority."""

    def _run(self, tmp, canary_log):
        (pathlib.Path(tmp) / "gate_canary.log").write_text(canary_log)
        text = GATE_SH.read_text()
        helper = _slice(text, _HELPER_START, _HELPER_END, "gate_sanitizer_verdict")
        block = _slice(text, _CANARY_BLOCK_START, _CANARY_BLOCK_END,
                       "gate.sh canary ASan leg")
        env = dict(os.environ)
        env["PIN_TMP"] = tmp
        script = (CANARY_HARNESS.replace("@@HELPER@@", helper)
                                .replace("@@BLOCK@@", block))
        return subprocess.run(["bash", "-c", script], cwd=str(REPO), env=env,
                              capture_output=True, text=True, timeout=60)

    def test_corruption_in_the_canary_fails_hard(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, GateBlockTest._log(**{"a.0": _scope("s.f", "fail", CORRUPTION)}))
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("CANARY_DUAL hard", r.stdout,
                          "the dual-consume marker must be recorded BEFORE the fail")
            self.assertIn("GATE_FAIL: canary ASan", r.stdout)

    def test_an_undeclared_leak_in_the_canary_fails_hard(self):
        """fix #4537: the canary consults the same gate_sanitizer_verdict as
        the authoritative pass, so an undeclared leak is evidence there too —
        the pre-#4537 "a leak never bricks the canary" behaviour is gone for
        the undeclared case; only a DECLARED leak stays inert (see below)."""
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, GateBlockTest._log(**{"a.0": _scope("s.l", "pass", LEAK)}))
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("CANARY_DUAL hard", r.stdout)
            self.assertIn("GATE_FAIL: canary ASan", r.stdout)
            self.assertIn("sanitizer_leak_exempt.txt", r.stdout)

    def test_a_declared_leak_in_the_canary_does_not_brick_the_gate(self):
        declared = ("class_field_record_string_release_4253."
                   "non_sealed_class_still_leaks_axis_sealed")
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, GateBlockTest._log(**{"a.0": _scope(declared, "pass", LEAK)}))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("AFTER_CANARY_BLOCK _c_asan=0", r.stdout,
                          "a DECLARED leak must not be handed to "
                          "gate.canary.decide as deterministic evidence — it "
                          "would FAIL_HARD")
            self.assertNotIn("CANARY_DUAL", r.stdout)

    def test_an_unreadable_canary_channel_degrades_open_and_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            r = self._run(tmp, "TOTAL: 120 passed, 0 failed\n")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn("not evaluated here", r.stdout)
            self.assertIn("#4533", r.stdout)
            self.assertIn("AFTER_CANARY_BLOCK _c_asan=0", r.stdout)


class MarkerContractTest(unittest.TestCase):
    """test_main.cpp's scope markers are the whole basis of attribution."""

    def test_the_markers_are_written_to_fd_2_not_stdout(self):
        text = TEST_MAIN.read_text()
        for needle in ('std::fprintf(stderr, "[san-scope] enter %s.%s\\n"',
                       'std::fprintf(stderr, "[san-scope] leave %s.%s %s\\n"'):
            self.assertIn(
                needle, text,
                "the markers must share the sanitizer report's UNBUFFERED fd 2; "
                "on std::cout they are block-buffered into a file and their "
                "order carries no information (#4533)")
        self.assertEqual(text.count("std::fflush(stderr)"), 2,
                         "each marker must be flushed, or the ordering the "
                         "attribution depends on is not guaranteed")

    def test_the_markers_are_gated_on_the_sweep(self):
        text = TEST_MAIN.read_text()
        self.assertIn("const bool sanScope = ::bezel::test::forceSanitizeAddress();",
                      text, "a normal single-binary run's stderr must be "
                            "byte-for-byte unchanged")
        # The stdout flush that makes the log readable must be gated the same
        # way: on the plain oracle it would change summary.py's documented
        # #1608 last-[ RUN ]-line culprit behaviour.
        body = text[text.index("const bool sanScope"):
                    text.index("        test.body();", text.index("const bool sanScope"))]
        self.assertIn("std::cout.flush();", body,
                      "the flush must live inside the sweep guard")
        self.assertEqual(text.count("std::cout.flush();"), 1)

    def test_the_marker_spelling_cannot_collide_with_the_RUN_culprit_grep(self):
        """summary.py's crashed-shard culprit extraction is anchored on
        `^[ RUN` (#1608). A marker starting with that prefix would silently
        change which test a crashed shard is said to have died on."""
        self.assertFalse(
            _summary._RUN_PREFIX_RE.match("[san-scope] enter suite.name"))
        self.assertFalse(
            _summary._RUN_PREFIX_RE.match("[san-scope] leave suite.name pass"))

    def test_the_scanner_and_test_main_agree_on_the_spelling(self):
        text = TEST_MAIN.read_text()
        self.assertIsNotNone(S.ENTER_RE.match("[san-scope] enter suite.name"))
        self.assertIsNotNone(S.LEAVE_RE.match("[san-scope] leave suite.name fail"))
        self.assertIn("[san-scope] enter", text)
        self.assertIn("[san-scope] leave", text)


class DeliberateReportCaptureTest(unittest.TestCase):
    """Rows that deliberately PRINT a headline must keep it out of the channel.
    driver_run_support_test.cpp's report row impersonates `ERROR:
    AddressSanitizer: heap-use-after-free` — exactly what the gate hard-fails
    on — and redirects fd 2 for the duration of the call. Inert before this
    fix, load-bearing after it."""

    def test_the_impersonated_report_row_still_redirects_fd_2(self):
        text = DRS_TEST.read_text()
        start = text.find("BEZEL_TEST(driver_run_support, "
                          "runProgramForTest_fails_the_test_on_a_sanitizer_report)")
        self.assertGreater(start, 0, "the report row has been renamed or removed")
        body = text[start:text.find("\n}\n", start)]
        self.assertIn("ERROR: AddressSanitizer", body,
                      "guard: this row is the one that prints a corruption headline")
        for needle in ("::dup(2)", "::dup2(captureFd, 2)", "::dup2(savedFd, 2)"):
            self.assertIn(
                needle, body,
                "dropping the fd-2 capture (%s) would plant a corruption "
                "headline in a shard log and red the cycle (#4533)" % needle)


class TautologyGuardTest(unittest.TestCase):
    """gate_full_cycle.sh's silent-degradation detector is
    `grep -qi 'asan\\|sanitize'` over gate.sh's output for the --full pass. The
    evidence line carries `SANITIZER`, so emitting it on a PLAIN run would
    satisfy that grep and turn the detector into a tautology — the same trap
    gate.sh's repo-identity banner documents."""

    def test_the_detector_still_greps_for_asan_or_sanitize(self):
        self.assertIn("grep -qi 'asan\\|sanitize'", CYCLE_SH.read_text(),
                      "guard: the detector this test protects has moved")

    def test_every_evidence_emitter_sits_behind_the_asan_guard(self):
        text = GATE_SH.read_text()
        regions = []
        for start, end, what in (
                (_HELPER_START, _HELPER_END, "helper"),
                (_ASAN_BLOCK_START, _ASAN_BLOCK_END, "shard ASan block"),
                (_CANARY_BLOCK_START, _CANARY_BLOCK_END, "canary ASan block")):
            block = _slice(text, start, end, what)
            first = text[:text.index(start)].count("\n") + 1
            regions.append(range(first, first + block.count("\n") + 1))
        calls = [n for n, ln in enumerate(text.splitlines(), 1)
                 if "gate_sanitizer_verdict" in ln
                 and not ln.strip().startswith("#")
                 and "gate_sanitizer_verdict() {" not in ln]
        self.assertEqual(
            len(calls), 2,
            "gate_sanitizer_verdict must have exactly two call sites (the "
            "authoritative pass and the canary); found lines %r" % calls)
        offenders = [n for n, ln in enumerate(text.splitlines(), 1)
                     if (S.EVIDENCE_PREFIX in ln or "gate_sanitizer_verdict" in ln)
                     and not ln.strip().startswith("#")
                     and not any(n in r for r in regions)]
        self.assertEqual(
            offenders, [],
            "every non-comment mention must live in the helper or in an "
            "`if [ \"$ASAN\" -eq 1 ]` block; emitting it on the plain oracle "
            "would make gate_full_cycle.sh's asan-ran detector a tautology. "
            "Offending lines: %r" % offenders)


class EvidenceContractTest(unittest.TestCase):
    """The lift is a contract between three files; pin the seams."""

    def test_run_parallel_calls_the_scanner_over_the_per_shard_logs(self):
        rp = RP_SH.read_text()
        self.assertIn('python3 -m gate.sanitizer scan "$logdir"', rp,
                      "the lift must read the PER-SHARD logs, not this script's "
                      "own stdout (#4533)")

    def test_the_lift_runs_inline_not_from_the_exit_trap(self):
        """_rp_keep is an EXIT trap, so any reader outside run_parallel sees the
        logs only on a FAILING run."""
        rp = RP_SH.read_text()
        self.assertLess(rp.index("sanitizer evidence lift"), rp.index("exit 0\n"))
        self.assertLess(rp.index("gate.sanitizer scan"),
                        rp.index("# ── AOT cache stats"),
                        "the scan must precede the run's teardown output")

    def test_a_scanner_failure_is_never_silent(self):
        rp = RP_SH.read_text()
        self.assertIn("gate.sanitizer scan FAILED", rp)


if __name__ == "__main__":
    unittest.main()
