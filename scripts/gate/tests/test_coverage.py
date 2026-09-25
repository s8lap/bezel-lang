"""Tests for gate.coverage (Phase 4 module, #3002 / #3011).

The load-bearing pins — each maps to an invariant the bash floor at
gate.sh:882-973 established and that a Python port must not regress:

  1. #3011 — a binary the manifest lists but that is NOT executable is a HARD
     failure (exit 3), never a silently-lowered expectation. Miss the codegen
     binary and `expected` would drop from 16482 to 5156, which is exactly the
     number the run that motivated #3002 reported as a pass.
  2. zero manifest rows => HARD fail (the registry lists nothing — not "zero
     tests expected").
  3. enumerate==0 => HARD fail (the registry read failed).
  4. manifest unreadable => HARD fail.
  5. ran < expected => coverage-short (exit 3, retry — NOT a real failure).
  6. ran >= expected => ok, and a SUPERSET is fine (== would be too strict).
  7. per-binary accounting: the {group: count} map sums correctly across rows.

Fixtures use a temp-dir manifest + injected runner/is_executable so no real
binary is built (stdlib only, hermetic). The injected runner returns
(rc, stdout_text); the line count is the enumerate protocol's one-test-per-line.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import coverage  # noqa: E402


def _row(group, path, count):
    """A manifest line: <group>\\t<path>\\t<#sources>. The #sources field is
    incidental to the floor (the count comes from BEZEL_TEST_ENUMERATE); it is
    kept so the parser exercises the real 3-field shape."""
    return "{}\t{}\t{}".format(group, path, count)


def _write_manifest(dirpath, rows):
    p = os.path.join(dirpath, "test_binaries.txt")
    with open(p, "w") as f:
        for r in rows:
            f.write(r + "\n")
    return p


class EnumerateExpectedTest(unittest.TestCase):
    def _runner(self, counts):
        """A runner that returns a canned enumerate line-count per binary path.

        `counts` maps binary path -> number of `suite.name\\n` lines to emit."""
        def run(binary):
            n = counts.get(binary, 0)
            return 0, "".join("Suite.Test{}\n".format(i) for i in range(n))
        return run

    def test_usable_enumeration_sums_per_binary(self):
        # The healthy case: two binaries, each enumerates; the per-binary map
        # and the total are exactly their counts.
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen"); b2 = os.path.join(d, "sema")
            mp = _write_manifest(d, [_row("codegen", b1, 9),
                                     _row("sema", b2, 7)])
            res = coverage.enumerate_expected(
                mp, runner=self._runner({b1: 4, b2: 6}),
                is_executable=lambda p: True)
        self.assertTrue(res.usable)
        self.assertEqual(res.rows, 2)
        self.assertEqual(res.total_expected, 10)
        self.assertEqual(res.per_binary_expected, {"codegen": 4, "sema": 6})
        self.assertEqual(res.errors, [])

    def test_missing_binary_is_hard_fail_not_lowered_bar(self):
        # per #3011: a non-executable binary is a HARD failure. Crucially the
        # verdict is unusable — the expectation is NOT silently reduced to the
        # one binary that IS present (the defect this floor exists to prevent).
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen"); b2 = os.path.join(d, "missing")
            mp = _write_manifest(d, [_row("codegen", b1, 9),
                                     _row("ownership", b2, 7)])
            res = coverage.enumerate_expected(
                mp, runner=self._runner({b1: 4, b2: 0}),
                is_executable=lambda p: p == b1)  # b2 is NOT executable
        self.assertFalse(res.usable)
        self.assertEqual(res.rows, 2)
        self.assertTrue(any("not executable" in e for e in res.errors))
        self.assertTrue(any("missing" in e for e in res.errors))

    def test_enumerate_zero_is_hard_fail(self):
        # per #3011: enumerate==0 is a HARD failure (the registry read failed),
        # never a 0-count expectation.
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen")
            mp = _write_manifest(d, [_row("codegen", b1, 9)])
            res = coverage.enumerate_expected(
                mp, runner=self._runner({b1: 0}),  # enumerates nothing
                is_executable=lambda p: True)
        self.assertFalse(res.usable)
        self.assertTrue(any("enumerated 0 tests" in e for e in res.errors))

    def test_zero_manifest_rows_is_hard_fail(self):
        # per #3011: a registry listing ZERO binaries is a HARD failure — the
        # manifest is missing the population, not "zero tests expected".
        with tempfile.TemporaryDirectory() as d:
            mp = _write_manifest(d, [])
            res = coverage.enumerate_expected(
                mp, runner=self._runner({}), is_executable=lambda p: True)
        self.assertFalse(res.usable)
        self.assertEqual(res.rows, 0)
        self.assertTrue(any("ZERO binaries" in e for e in res.errors))

    def test_unreadable_manifest_is_hard_fail(self):
        # gate.sh:943-945 — an unreadable manifest cannot establish what SHOULD
        # run, so the floor is unusable (never silently skipped).
        with tempfile.TemporaryDirectory() as d:
            res = coverage.enumerate_expected(
                os.path.join(d, "does_not_exist.txt"),
                runner=self._runner({}), is_executable=lambda p: True)
        self.assertFalse(res.usable)
        self.assertTrue(res.errors)

    def test_crash_after_partial_output_counts_what_was_printed(self):
        # Faithful to `wc -l` + ignored rc: a binary that prints 3 lines then
        # crashes (rc!=0) still counts 3 — the bash never tests the enumerate
        # exit code. (The #sources field is unrelated to the count.)
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen")
            mp = _write_manifest(d, [_row("codegen", b1, 9)])
            res = coverage.enumerate_expected(
                mp, runner=lambda b: (139, "A.x\nB.y\nC.z\n"),
                is_executable=lambda p: True)
        self.assertTrue(res.usable)
        self.assertEqual(res.total_expected, 3)


class FloorOkTest(unittest.TestCase):
    def test_ran_below_expected_is_coverage_short_exit3(self):
        # per #3002: a short run is exit 3 (retry), not 1 (real fail) — a
        # truncated run says nothing about the tree either way.
        v = coverage.floor_ok(5156, 16482, {}, {"codegen": 16482})
        self.assertFalse(v.ok)
        self.assertEqual(v.verdict, "coverage-short")
        self.assertEqual(v.exit_code, coverage.EXIT_RETRY)
        self.assertIn("31%", v.message)  # 5156/16482 == 31%

    def test_ran_equal_expected_is_ok(self):
        v = coverage.floor_ok(16482, 16482, {}, {})
        self.assertTrue(v.ok)
        self.assertEqual(v.verdict, "ok")
        self.assertEqual(v.exit_code, coverage.EXIT_OK)

    def test_ran_above_expected_superset_is_ok(self):
        # == would be too strict: a re-run that picks up a few extra tests is
        # not a defect. A SUPERSET is fine.
        v = coverage.floor_ok(16500, 16482, {}, {})
        self.assertTrue(v.ok)
        self.assertEqual(v.verdict, "ok")

    def test_unusable_registry_is_hard_fail_regardless_of_ran(self):
        # The #3011 proof: even if `ran` happens to meet the (secretly lowered)
        # expected, an unusable registry is a HARD fail — the comparison must
        # not run against a bar that was quietly dropped.
        v = coverage.floor_ok(16482, 5156, {}, {"codegen": 5156},
                              usable=False, errors=["codegen not executable"])
        self.assertFalse(v.ok)
        self.assertEqual(v.verdict, "registry-unusable")
        self.assertEqual(v.exit_code, coverage.EXIT_RETRY)
        self.assertIn("codegen not executable", v.message)

    def test_per_binary_accounting_carried_through(self):
        v = coverage.floor_ok(100, 100,
                              {"codegen": 60, "sema": 40},
                              {"codegen": 60, "sema": 40})
        self.assertTrue(v.ok)
        self.assertEqual(v.per_binary_ran, {"codegen": 60, "sema": 40})
        self.assertEqual(v.per_binary_expected, {"codegen": 60, "sema": 40})

    def test_issue_3737_repro_stays_coverage_short_pending_3002_ruling(self):
        # #3737's own shape: TOTAL 20573 passed, 2 failed, both arbitrated
        # FLAKY. The floor compares PASSED, not attempted — #3002's deliberate
        # choice (a cleared failure also leaves passed < expected). The ask to
        # fold arbitration suspects into `ran` was declined-with-reasons
        # pending a #3002 policy ruling, so this must stay coverage-short.
        v = coverage.floor_ok(20573, 20575, {}, {})
        self.assertFalse(v.ok)
        self.assertEqual(v.verdict, "coverage-short")
        self.assertEqual(v.ran, 20573)

    def test_floor_takes_no_accepted_flaky_fold(self):
        # Pins the decline: floor_ok has no side channel that credits
        # arbitration suspects (which may include un-re-run capped suspects
        # and `died on:` victims already counted in passed).
        with self.assertRaises(TypeError):
            coverage.floor_ok(26252, 26538, {}, {}, accepted_flaky=286)


class CheckCoverageFloorTest(unittest.TestCase):
    def test_end_to_end_ok(self):
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen")
            mp = _write_manifest(d, [_row("codegen", b1, 5)])
            v = coverage.check_coverage_floor(
                5, {"codegen": 5}, mp,
                runner=lambda b: (0, "A.b\nC.d\nE.f\nG.h\nI.j\n"),
                is_executable=lambda p: True)
        self.assertTrue(v.ok)
        self.assertEqual(v.verdict, "ok")

    def test_end_to_end_missing_binary_hard_fail(self):
        # The integrated path: a missing binary makes the WHOLE check a hard
        # fail, even though the one present binary's count would otherwise pass.
        with tempfile.TemporaryDirectory() as d:
            b1 = os.path.join(d, "codegen"); b2 = os.path.join(d, "absent")
            mp = _write_manifest(d, [_row("codegen", b1, 5),
                                     _row("sema", b2, 5)])
            v = coverage.check_coverage_floor(
                9999, {}, mp,
                runner=lambda b: (0, "A.b\nB.c\nC.d\nD.e\nE.f\n"),
                is_executable=lambda p: p == b1)
        self.assertFalse(v.ok)
        self.assertEqual(v.verdict, "registry-unusable")
        self.assertEqual(v.exit_code, coverage.EXIT_RETRY)


def _fake_binary(dirpath, name, ntests):
    """A hermetic stand-in for a gtest binary: prints `ntests` test-name lines
    when run (the BEZEL_TEST_ENUMERATE protocol prints one suite.name per line).
    The real `_default_enumerate_runner` execs this and counts the newlines."""
    p = os.path.join(dirpath, name)
    with open(p, "w") as f:
        f.write("#!/bin/sh\n")
        for i in range(ntests):
            f.write("echo Suite%d.test%d\n" % (i % 3, i))
    os.chmod(p, 0o755)
    return p


class CLITest(unittest.TestCase):
    """`python3 -m gate.coverage check` — the Wave-2 gate.sh bridge (#3010).

    The EXIT CODE is the decision (0 ok / 3 retry); stdout carries the banner
    plus `@cov:` machine lines. Pins: exit 0 on a complete run, exit 3 on a
    coverage-short AND on a registry-unusable shape, and that the @cov machine
    lines carry the values the orchestrator harvests for telemetry.
    """

    _SCRIPTS_DIR = os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", ".."))

    def _run_cli(self, args):
        import subprocess
        env = dict(os.environ, PYTHONPATH=self._SCRIPTS_DIR)
        return subprocess.run(
            [sys.executable, "-m", "gate.coverage"] + args,
            capture_output=True, text=True, env=env)

    def _machine(self, out):
        """Parse the `@cov:k=v` lines the orchestrator harvests."""
        m = {}
        for line in out.splitlines():
            if line.startswith("@cov:"):
                k, v = line[len("@cov:"):].split("=", 1)
                m[k] = v
        return m

    def test_check_ok_exits_zero_and_emits_floor_ok(self):
        with tempfile.TemporaryDirectory() as d:
            b1 = _fake_binary(d, "codegen", 5)
            b2 = _fake_binary(d, "sema", 3)
            mp = _write_manifest(d, [_row("codegen", b1, 99),
                                     _row("sema", b2, 88)])
            r = self._run_cli(["check", "--ran", "8", "--manifest", mp])
            self.assertEqual(r.returncode, 0, r.stderr)
            m = self._machine(r.stdout)
            self.assertEqual(m["exit"], "0")
            self.assertEqual(m["floor_ok"], "1")
            self.assertEqual(m["expected"], "8")
            self.assertIn("codegen:5", m["bin_expected"])
            self.assertIn("sema:3", m["bin_expected"])

    def test_check_rejects_accepted_flaky_flag(self):
        # #3737 decline: the CLI must not accept a suspect fold (argparse
        # usage error, exit 2) — reintroducing it needs a #3002 ruling.
        with tempfile.TemporaryDirectory() as d:
            b1 = _fake_binary(d, "codegen", 5)
            mp = _write_manifest(d, [_row("codegen", b1, 99)])
            r = self._run_cli(["check", "--ran", "3", "--manifest", mp,
                               "--accepted-flaky", "2"])
            self.assertEqual(r.returncode, 2, r.stdout + r.stderr)

    def test_gate_sh_feeds_passed_count_and_keeps_gtests_meaning(self):
        # #3002 wiring: gate.sh passes the in-shard PASSED total as --ran, adds
        # no suspect fold, and does not overwrite COV_GTESTS (the telemetry
        # `gtests` field) from the coverage module's output.
        gate_sh = open(os.path.join(self._SCRIPTS_DIR, "gate.sh")).read()
        self.assertIn('--ran "${COV_GTESTS:-0}"', gate_sh)
        self.assertNotIn("--accepted-flaky", gate_sh)
        self.assertNotIn("@cov:ran=", gate_sh)
        self.assertNotRegex(gate_sh, r"(?m)^[^#]*\bCOV_GTESTS=\$_cov")

    def test_check_ran_superset_of_expected_is_ok(self):
        # A re-run that picks up a few extra tests is NOT a defect (== too strict).
        with tempfile.TemporaryDirectory() as d:
            b1 = _fake_binary(d, "codegen", 5)
            mp = _write_manifest(d, [_row("codegen", b1, 99)])
            r = self._run_cli(["check", "--ran", "6", "--manifest", mp])
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_check_coverage_short_exits_three(self):
        with tempfile.TemporaryDirectory() as d:
            b1 = _fake_binary(d, "codegen", 500)
            mp = _write_manifest(d, [_row("codegen", b1, 99)])
            r = self._run_cli(["check", "--ran", "100", "--manifest", mp])
            self.assertEqual(r.returncode, 3, r.stdout)
            m = self._machine(r.stdout)
            self.assertEqual(m["exit"], "3")
            self.assertEqual(m["floor_ok"], "0")
            self.assertIn("COVERAGE SHORT", r.stdout)
            self.assertEqual(m["failed_phase"],
                             "coverage short: 100 of 500 gtests ran")

    def test_check_missing_binary_hard_fails_exit_three(self):
        # #3011: a non-executable registry binary is a HARD fail, never a
        # silently-lowered expectation. The CLI inherits the default
        # is_executable, so a path that was never made executable triggers it.
        with tempfile.TemporaryDirectory() as d:
            b1 = _fake_binary(d, "codegen", 5)
            missing = os.path.join(d, "never-built")
            mp = _write_manifest(d, [_row("codegen", b1, 99),
                                     _row("sema", missing, 88)])
            r = self._run_cli(["check", "--ran", "9999", "--manifest", mp])
            self.assertEqual(r.returncode, 3, r.stdout)
            m = self._machine(r.stdout)
            self.assertEqual(m["floor_ok"], "0")
            self.assertIn("not executable", r.stdout)

    def test_check_zero_rows_hard_fails(self):
        with tempfile.TemporaryDirectory() as d:
            mp = os.path.join(d, "test_binaries.txt")
            open(mp, "w").close()
            r = self._run_cli(["check", "--ran", "0", "--manifest", mp])
            self.assertEqual(r.returncode, 3, r.stdout)
            self.assertIn("ZERO binaries", r.stdout)

    def test_no_subcommand_exits_2(self):
        r = self._run_cli([])
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()
