"""Tests for gate.arbitration (Phase 4 module, #1861 / #2100).

The load-bearing pins — the FIVE INVARIANTS a Python port of gate.sh:432-524
must preserve verbatim. Each is the lesson of a gate that PASSED RED, made
structural here so the Wave-2 wiring cannot regress it:

  1. ran-nothing is NOT a pass — a re-run that exits 0 having run 0 tests
     validates nothing (arb_run_suite's total>0 guard).
  2. [ FAIL ] never sampled away — every failing suite is arbitrated IN FULL;
     only `died on:` (timeout-victim) suites are subject to the sample cap,
     and only AFTER every [ FAIL ] is checked (the gate-E lesson).
  3. `died on: <none>` stripped — re-running "<none>" matches nothing and would
     launder a dropped slice into a flake-ACCEPT.
  4. return 2 never collapsed to 0 — a marker-less nonzero exit is
     undercount/timeout, which the caller distrusts; it is never a pass.
  5. ARB_ENV forces ASan in ASan modes so an ASan-only failure reproduces.

Plus the faithful-port details: fail-overlap removal (comm-13), fail-fast on
the first nonzero-exit binary, the last-summary-line harvest, and the 0/1
verdict split. `run_suite` and the binary runner are injected stubs so no real
binary is built (stdlib only, hermetic).
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import arbitration as arb  # noqa: E402
from gate import summary  # noqa: E402  (fix #3668 banner cross-check)


def _fail_line(test):
    return "[ FAIL ] {} (0 ms)".format(test)


def _died_line(test):
    # The real shard-log format (run_parallel.sh:320):
    #   "##   shard <s> exited nonzero — died on: <lasttest or <none>>"
    # `sed 's/.*died on: //'` captures exactly the test name (no trailing junk).
    return "##   shard codegen.0 exited nonzero — died on: {}".format(test)


class _RunSuiteRecorder:
    """A stub run_suite(suite)->bool that records every call in order.

    `result_for` maps suite name -> bool; a suite absent from the map defaults
    to True (the clean-flake case). `calls` is the ordered list of suites
    actually re-run, so a test can assert EXACTLY which class was arbitrated
    and how many times each was tried."""

    def __init__(self, result_for=None):
        self.result_for = dict(result_for or {})
        self.calls = []

    def __call__(self, suite):
        self.calls.append(suite)
        return self.result_for.get(suite, True)


class ParseTest(unittest.TestCase):
    def test_parse_crashed_extracts_victim_after_marker(self):
        # Faithful to `sed -E 's/.*died on: //'`: the text after `died on: ` is
        # the victim test name (run_parallel.sh:320's real format).
        log = ("junk\n"
               "##   shard codegen.0 exited nonzero — died on: OwnBranch.DisposalA\n"
               "no marker here\n"
               "##   shard sema.1 exited nonzero — died on: Lexer.Strings\n")
        self.assertEqual(arb.parse_crashed(log),
                         ["OwnBranch.DisposalA", "Lexer.Strings"])

    def test_parse_crashed_keeps_none_literal(self):
        # <none> is KEPT by parse_crashed (the caller strips it); the raw parse
        # is faithful to `sed 's/.*died on: //'`.
        self.assertEqual(arb.parse_crashed("died on: <none>\n"), ["<none>"])

    def test_parse_failed_takes_first_token(self):
        log = ("[ FAIL ] OwnBranch.DisposalA (0 ms)\n"
               "[ FAIL ] Lexer.Strings [some detail] (1 ms)\n"
               "not a fail line\n")
        self.assertEqual(arb.parse_failed(log),
                         ["OwnBranch.DisposalA", "Lexer.Strings"])

    def test_parse_summary_takes_last_line(self):
        # Faithful to `grep ... | tail -1`: a stray earlier match must not win.
        out = ("1 passed, 0 failed, 1 total\n"
               "[ RUN ] x.y\n"
               "9 passed, 2 failed (shard 0/2)\n")
        self.assertEqual(arb._parse_summary_last(out), 11)

    def test_parse_summary_no_match_is_zero(self):
        self.assertEqual(arb._parse_summary_last("nothing here\n"), 0)
        self.assertEqual(arb._parse_summary_last(""), 0)


class BannerToSuspectTest(unittest.TestCase):
    """fix #3668: the shard-log text the gate feeds `arbitrate` is
    run_parallel's own output — the `--- failing tests ---` block plus the
    GATE FAILURE banner. This pins the JOIN between the two modules: what
    summary.format_failure_banner writes is what arbitration.parse_* reads.

    Before the fix, a shard that ran its whole slice and failed ONE test still
    got a `died on: <last passing test>` line, so parse_crashed handed
    arbitration an innocent suspect; it was re-run 5x under ASan, passed, and
    was stamped FLAKY (Run 10 cycle-1, shard module/7). arbitration.py itself
    is unchanged — its `[ FAIL ]` parsing already yields the real suspect —
    so the join is the whole repair.
    """

    INNOCENT = ("xmod_with_replacement_type_3177."
                "imported_positional_record_with_target_is_type_checked_9_5")
    REAL = ("xmod_abstract_class_indexer_unoverridden_row12."
            "cross_module_indexer_row12")

    def _gate_log(self, missing):
        """run_parallel's output shape: the failing-test block it always
        prints when any shard log carries a [ FAIL ] line
        (tests/run_parallel.sh:317-322), then the banner."""
        banner = summary.format_failure_banner(
            ran=1522, expected=1523, nbin=6,
            undercount_bins=["module: ran 1522 of 1523 registered"],
            subset_empty=False, missing_summary_shards=missing, njobs=48,
            crashed_culprits=[("module/7", self.INNOCENT)],
            crashed_log_failures=[("module/7", [self.REAL])])
        return "--- failing tests ---\n" + _fail_line(self.REAL) + "\n" + banner

    def test_completed_shard_contributes_no_crash_suspect(self):
        log = self._gate_log(missing=[])
        # No died-on victim at all => the innocent suite is never re-run.
        self.assertEqual(arb.parse_crashed(log), [])
        # ...but the marker arbitration needs is still there: the REAL test.
        self.assertEqual(arb.parse_failed(log), [self.REAL])

    def test_completed_shard_arbitrates_only_the_real_suite(self):
        # End-to-end through arbitrate(): exactly one suite is re-run, the
        # failing one, and a suite that fails alone is a REAL regression.
        rec = _RunSuiteRecorder({arb._suite_name(self.REAL): False})
        res = arb.arbitrate(self._gate_log(missing=[]), run_suite=rec)
        self.assertEqual(res.verdict, arb.ARB_REAL)
        self.assertEqual(res.fail_suites, [arb._suite_name(self.REAL)])
        self.assertEqual(res.crash_suites, [])
        self.assertNotIn(arb._suite_name(self.INNOCENT), rec.calls)

    def test_crashed_shard_still_contributes_its_died_on_suspect(self):
        # The narrowing must not blind arbitration to a REAL crash: a shard
        # with no completion summary keeps its died-on victim as a suspect,
        # because for a true crash the victim can be the culprit.
        log = self._gate_log(missing=["module/7"])
        self.assertEqual(arb.parse_crashed(log), [self.INNOCENT])
        self.assertEqual(arb.parse_failed(log), [self.REAL])

    def test_marker_never_disappears_so_verdict_two_is_not_manufactured(self):
        # Invariant 4: a nonzero exit with NO crash/FAIL marker returns 2 and
        # BAILs the gate. Suppressing the died-on line must not create that
        # state — the [ FAIL ] line is always present in the same log
        # (run_parallel greps the same `^[ FAIL ]` lines the banner's
        # actual-FAIL list came from), so a suspect always remains.
        rec = _RunSuiteRecorder()
        res = arb.arbitrate(self._gate_log(missing=[]), run_suite=rec)
        self.assertNotEqual(res.verdict, arb.ARB_UNTRUSTWORTHY)
        self.assertEqual(res.verdict, arb.ARB_FLAKY)
        self.assertEqual(res.last_suspects, 1)


class ArbRunSuiteTest(unittest.TestCase):
    def _runner(self, per_binary):
        """per_binary: {binary: (rc, stdout)}. Records call order."""
        calls = []

        def run(binary, suite, arb_env, timeout):
            calls.append((binary, suite, dict(arb_env or {})))
            rc, out = per_binary.get(binary, (0, ""))
            return rc, out
        return run, calls

    def test_passes_when_one_binary_ran_tests_and_all_exit_zero(self):
        run, _ = self._runner({
            "binA": (0, "5 passed, 0 failed\n"),
            "binB": (0, ""),  # holds none of the suite -> walks registry, 0 ran
        })
        self.assertTrue(arb.arb_run_suite(
            "SomeSuite", ["binA", "binB"], runner=run))

    def test_ran_nothing_is_not_a_pass(self):
        # Invariant 1: every binary exits 0 but NO summary line ran (the
        # suspect name matched nowhere). Without the total>0 guard this would
        # be a flake-ACCEPT — the exact false-green the gate exists to prevent.
        run, _ = self._runner({"binA": (0, ""), "binB": (0, "")})
        self.assertFalse(arb.arb_run_suite(
            "MatchesNothing", ["binA", "binB"], runner=run))

    def test_fail_fast_on_first_nonzero_exit(self):
        # Faithful to gate.sh:450 `[ "$rc" -ne 0 ] && return 1`: the second
        # binary is never even launched once the first exits nonzero.
        run, calls = self._runner({
            "binA": (1, "boom\n"),
            "binB": (0, "5 passed, 0 failed\n"),
        })
        self.assertFalse(arb.arb_run_suite(
            "SomeSuite", ["binA", "binB"], runner=run))
        self.assertEqual(len(calls), 1)  # binB was NOT launched
        self.assertEqual(calls[0][0], "binA")

    def test_arb_env_is_passed_to_runner(self):
        # Invariant 5: ARB_ENV forces ASan so an ASan-only failure reproduces.
        # The runner receives arb_env and is responsible for merging it into
        # the subprocess environment alongside BEZEL_TEST_FILTER.
        run, calls = self._runner({"binA": (0, "1 passed, 0 failed\n")})
        arb.arb_run_suite("S", ["binA"], runner=run,
                          arb_env={"BEZEL_AOT_FORCE_SANITIZE": "address"})
        self.assertEqual(calls[0][2], {"BEZEL_AOT_FORCE_SANITIZE": "address"})

    def test_blank_binary_paths_skipped(self):
        run, calls = self._runner({"binA": (0, "1 passed, 0 failed\n")})
        arb.arb_run_suite("S", ["", "binA", ""], runner=run)
        self.assertEqual([c[0] for c in calls], ["binA"])


class ArbitrateStructureTest(unittest.TestCase):
    def test_fail_suites_arbitrated_in_full_crash_subject_to_cap(self):
        # INVARIANT 2 — the gate-E lesson. 10 [FAIL] suites whose names sort
        # AFTER 20 `died on:` crash suites. If the cap applied alphabetically
        # (the old bug), the 10 real failures (zfail*) would be sampled away
        # and only aaa0..aaa7 would be re-run. The port lists fail_suites
        # FIRST, so ALL 10 zfail* are arbitrated and only 8 of 20 aaa* are.
        fails = ["zfail{}.T".format(i) for i in range(10)]
        crashes = ["aaa{}.T".format(i) for i in range(20)]
        log = "\n".join(_fail_line(f) for f in fails) + "\n" + \
              "\n".join(_died_line(c) for c in crashes) + "\n"

        rec = _RunSuiteRecorder()  # all flaky
        res = arb.arbitrate(log, rec, sample_cap=8)

        self.assertEqual(res.verdict, arb.ARB_FLAKY)
        self.assertEqual(res.fail_suites,
                         sorted("zfail{}".format(i) for i in range(10)))
        self.assertEqual(res.crash_suites,
                         sorted("aaa{}".format(i) for i in range(20)))
        # Every zfail suite was re-run; exactly 8 aaa suites were (the cap).
        zfail_run = [s for s in res.sampled if s.startswith("zfail")]
        aaa_run = [s for s in res.sampled if s.startswith("aaa")]
        self.assertEqual(len(zfail_run), 10)
        self.assertEqual(len(aaa_run), 8)
        # The cap message names the crash-only count and states the invariant.
        self.assertTrue(any("capped arbitration at 8 of 20" in m for m in res.messages))
        self.assertTrue(any("real [ FAIL ] was arbitrated in full" in m
                            for m in res.messages))

    def test_cap_only_truncates_crash_suites_not_fail_suites(self):
        # A focused invariant-2 proof: even with crash suites overflowing the
        # cap, a fail suite is NEVER truncated. One [FAIL] + many crashes.
        crashes = ["c{}.T".format(i) for i in range(20)]
        log = _fail_line("RealFail.T") + "\n" + \
              "\n".join(_died_line(c) for c in crashes) + "\n"
        rec = _RunSuiteRecorder()
        res = arb.arbitrate(log, rec, sample_cap=8)
        self.assertIn("RealFail", res.sampled)
        self.assertEqual(len([s for s in res.sampled if s.startswith("c")]), 8)

    def test_fail_overlap_removed_from_crash_suites(self):
        # gate.sh:491-493 (comm -13): a suite that has a [ FAIL ] needs no
        # second arbitration, so it is removed from crash_suites. Foo fails
        # outright ([ FAIL ] Foo.A) AND a shard died on another Foo test
        # (Foo.B) -> Foo is arbitrated ONCE, as a fail_suite; it is dropped
        # from crash_suites. Bar.C stays a crash suite.
        log = (_fail_line("Foo.A") + "\n" + _died_line("Foo.B") + "\n" +
               _died_line("Bar.C") + "\n")
        rec = _RunSuiteRecorder()
        res = arb.arbitrate(log, rec)
        self.assertEqual(res.fail_suites, ["Foo"])
        self.assertNotIn("Foo", res.crash_suites)  # removed (already a fail)
        self.assertEqual(res.crash_suites, ["Bar"])
        # Foo is arbitrated exactly once (as a fail_suite), not twice.
        self.assertEqual(rec.calls.count("Foo"), 1)

    def test_real_regression_when_a_sampled_crash_suite_fails_alone(self):
        # The cap bounds cost but still surfaces a genuine widespread
        # regression: a sampled crash suite that fails all 5 tries => REAL.
        crashes = ["c{}.T".format(i) for i in range(3)]
        log = "\n".join(_died_line(c) for c in crashes) + "\n"
        rec = _RunSuiteRecorder({"c1": False})  # c1 is a real failure
        res = arb.arbitrate(log, rec, sample_cap=8)
        self.assertEqual(res.verdict, arb.ARB_REAL)
        self.assertTrue(any("REAL: c1" in m for m in res.messages))

    def test_flaky_suite_retried_only_until_first_pass(self):
        # gate.sh:509-514 — up to 5 tries, breaking on the first pass. A suite
        # that passes on try 1 is re-run ONCE, not five times.
        log = _fail_line("Flaky.A") + "\n"
        attempts = {"count": 0}

        def run(suite):
            attempts["count"] += 1
            return True  # passes first try
        res = arb.arbitrate(log, run)
        self.assertEqual(res.verdict, arb.ARB_FLAKY)
        self.assertEqual(attempts["count"], 1)

    def test_real_suite_tries_all_five(self):
        log = _fail_line("RealFail.A") + "\n"
        attempts = {"count": 0}

        def run(suite):
            attempts["count"] += 1
            return False  # never passes
        res = arb.arbitrate(log, run)
        self.assertEqual(res.verdict, arb.ARB_REAL)
        self.assertEqual(attempts["count"], 5)


class ArbitrateInvariantsTest(unittest.TestCase):
    def test_died_on_none_is_stripped(self):
        # INVARIANT 3 — `<none>` is a shard killed before running ANY test.
        # Re-running "<none>" matches nothing, "passes", and would launder a
        # dropped whole slice into a flake-ACCEPT. It is stripped.
        log = (_died_line("<none>") + "\n" + _died_line("Real.C") + "\n")
        rec = _RunSuiteRecorder()
        res = arb.arbitrate(log, rec)
        self.assertEqual(res.crash_suites, ["Real"])
        self.assertNotIn("<none>", res.crash_suites)
        self.assertEqual(res.last_suspects, 1)  # <none> excluded from the count

    def test_only_none_victims_is_return_two_not_laundered(self):
        # A whole slice dropped at startup -> only `died on: <none>`. After
        # stripping there are NO arbitrable suspects -> return 2 (the caller
        # distrusts), NEVER return 0 (a pass). This is the <none>-laundering
        # defense in its purest form.
        log = _died_line("<none>") + "\n"
        rec = _RunSuiteRecorder()
        res = arb.arbitrate(log, rec)
        self.assertEqual(res.verdict, arb.ARB_UNTRUSTWORTHY)
        self.assertEqual(res.sampled, [])  # nothing re-run

    def test_empty_log_is_return_two_not_collapsed_to_zero(self):
        # INVARIANT 4 — a nonzero exit with NO crash/FAIL marker is an
        # undercount/timeout shape. The caller treats 2 as untrustworthy
        # (gate_full_cycle distrusts it); it is NEVER collapsed into 0.
        res = arb.arbitrate("rc=1 but no markers anywhere\n", _RunSuiteRecorder())
        self.assertEqual(res.verdict, arb.ARB_UNTRUSTWORTHY)
        self.assertEqual(res.last_suspects, 0)
        self.assertEqual(res.sampled, [])

    def test_all_flaky_returns_zero(self):
        log = _fail_line("A.x") + "\n" + _died_line("B.y") + "\n"
        res = arb.arbitrate(log, _RunSuiteRecorder())
        self.assertEqual(res.verdict, arb.ARB_FLAKY)
        self.assertEqual(sorted(res.sampled), ["A", "B"])

    def test_last_suspects_counts_unique_test_level_suspects(self):
        # ARB_LAST_SUSPECTS is the unique TEST-level suspect count (crashed +
        # failed, minus <none>), NOT the suite-level count. Two [FAIL]s in one
        # suite + one died-on crash in another = 3 test-level suspects, 2 suites.
        log = (_fail_line("Same.FA") + "\n" + _fail_line("Same.FB") + "\n" +
               _died_line("Other.C") + "\n")
        res = arb.arbitrate(log, _RunSuiteRecorder())
        self.assertEqual(res.last_suspects, 3)
        self.assertEqual(res.fail_suites, ["Same"])   # both FAILs are suite Same
        self.assertEqual(res.crash_suites, ["Other"])  # Other.C is a crash victim


class CLITest(unittest.TestCase):
    """`python3 -m gate.arbitration {arbitrate|suspects}` — Wave-2 bridge (#3010).

    The `suspects` subcommand is the cheap dual-run probe (records the sample
    order WITHOUT re-running). The `arbitrate` subcommand is only reachable on
    the exit-2 (no-suspects) path without real binaries — its re-run logic is
    pinned by the function tests above; here we pin its CLI contract.
    """

    _SCRIPTS_DIR = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ".."))

    def _run_cli(self, args):
        import subprocess
        env = dict(os.environ, PYTHONPATH=self._SCRIPTS_DIR)
        return subprocess.run(
            [sys.executable, "-m", "gate.arbitration"] + args,
            capture_output=True, text=True, env=env)

    def _write_manifest(self, d):
        mp = os.path.join(d, "test_binaries.txt")
        with open(mp, "w") as f:
            f.write("codegen\t%s/b1\t10\n" % d)
        return mp

    def _machine(self, out):
        m = {}
        for line in out.splitlines():
            if line.startswith("@arb:"):
                k, v = line[len("@arb:"):].split("=", 1)
                m[k] = v
        return m

    def test_suspects_orders_fail_suites_before_crash_and_caps_crash(self):
        # invariant 2: every [FAIL] suite is arbitrated IN FULL; only the crash
        # (timeout-victim) tail is capped. The recording stub observes the order.
        # The crash victims span DISTINCT suites (Crash0..Crash11) so the cap,
        # which counts crash SUITES (deduped, like the bash `sort -u`), fires.
        log = "\n".join([
            _fail_line("OwnBranch.A"),
            _fail_line("OwnBranch.B"),
        ] + [_died_line("Crash%d.x" % i) for i in range(12)]) + "\n"
        with tempfile.TemporaryDirectory() as d:
            lf = os.path.join(d, "shard.log")
            with open(lf, "w") as f:
                f.write(log)
            r = self._run_cli(
                ["suspects", "--log", lf, "--sample-cap", "8"])
            self.assertEqual(r.returncode, 0, r.stderr)
            m = self._machine(r.stdout)
            self.assertEqual(m["fail_suites"], "OwnBranch")
            self.assertEqual(m["cap_fired"], "1")
            sampled = m["sampled"].split()
            # OwnBranch first, then crash suites up to the cap (8).
            self.assertEqual(sampled[0], "OwnBranch")
            self.assertEqual(len(sampled), 1 + 8)

    def test_suspects_strips_none_and_reports_untrustworthy_when_empty(self):
        # <none> is stripped (invariant 3); an all-<none> log has no arbitrable
        # suspect ⇒ the caller treats the result as untrustworthy (last_suspects=0).
        log = _died_line("<none>") + "\n"
        with tempfile.TemporaryDirectory() as d:
            lf = os.path.join(d, "shard.log")
            with open(lf, "w") as f:
                f.write(log)
            r = self._run_cli(["suspects", "--log", lf])
            self.assertEqual(r.returncode, 0, r.stderr)
            m = self._machine(r.stdout)
            self.assertEqual(m["last_suspects"], "0")
            self.assertEqual(m["fail_suites"], "")
            self.assertEqual(m["sampled"], "")

    def test_suspects_no_cap_when_crash_count_under_limit(self):
        # Crash victims in the SAME suite dedupe to ONE crash suite (matching the
        # bash `sort -u` on the suite-stripped name), so 2 crashes < cap ⇒ no cap.
        log = "\n".join([_fail_line("F.X"), _died_line("C.a"), _died_line("C.b")])
        with tempfile.TemporaryDirectory() as d:
            lf = os.path.join(d, "shard.log")
            with open(lf, "w") as f:
                f.write(log)
            r = self._run_cli(["suspects", "--log", lf, "--sample-cap", "8"])
            m = self._machine(r.stdout)
            self.assertEqual(m["cap_fired"], "0")
            self.assertEqual(m["sampled"], "F C")

    def test_arbitrate_no_suspects_exits_two(self):
        # A clean log (nonzero exit but no crash/FAIL marker) ⇒ exit 2
        # (untrustworthy). arbitrate returns before re-running anything, so a
        # real binary is not needed (only a valid manifest).
        with tempfile.TemporaryDirectory() as d:
            mp = self._write_manifest(d)
            lf = os.path.join(d, "shard.log")
            with open(lf, "w") as f:
                f.write("## GATE FAILURE: shard crash / undercount\n")
            r = self._run_cli(["arbitrate", "--log", lf, "--manifest", mp])
            self.assertEqual(r.returncode, 2, r.stdout)
            self.assertIn("@arb:suspects=0", r.stdout)

    def test_no_subcommand_exits_2(self):
        r = self._run_cli([])
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
