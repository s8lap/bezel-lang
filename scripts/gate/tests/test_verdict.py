"""Tests for gate.verdict (#3002, Phase 3 — authoritative verdict file).

The three load-bearing pins the task requires:
  1. trustworthy=False IFF pass+truncated — the #3002 shape (gate says pass
     over a truncated run; the trustworthy flag is the safety net).
  2. the verdict enum covers every gate.sh exit path (fail/retry/refused/
     poisoned/pass) — faithful mapping, no missing or extra states.
  3. the file is valid JSON + present after a simulated mid-exit crash
     (atomic rename: os.replace raises -> target unchanged/absent).

Plus: the build_verdict_obj schema pin (every field present, correct types),
the CLI smoke test (gate.sh's entry point), and read_verdict graceful
degradation on garbage/missing input.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import verdict  # noqa: E402


class VerdictFromRcTest(unittest.TestCase):
    """Every gate.sh exit code maps to the correct Verdict (the exit-code
    taxonomy: 0 pass / 1 fail / 2 refused / 3 retry / 64 refused)."""

    def test_rc0_not_truncated_is_pass(self):
        self.assertEqual(verdict.verdict_from_rc(0), verdict.Verdict.PASS)

    def test_rc0_truncated_is_poisoned(self):
        # per #3002: rc=0 over a truncated run is POISONED, not PASS.
        self.assertEqual(verdict.verdict_from_rc(0, truncated=True),
                         verdict.Verdict.POISONED)

    def test_rc1_is_fail(self):
        self.assertEqual(verdict.verdict_from_rc(1), verdict.Verdict.FAIL)

    def test_rc2_is_refused(self):
        # Preflight: zero-builders rule (gate.sh:113) or not a git repo (:71).
        self.assertEqual(verdict.verdict_from_rc(2), verdict.Verdict.REFUSED)

    def test_rc3_is_retry(self):
        # per #3002: coverage short — transient, gate_full_cycle retries.
        self.assertEqual(verdict.verdict_from_rc(3), verdict.Verdict.RETRY)

    def test_rc64_is_refused(self):
        # per #3011: unrecognized mode (gate.sh:95) — a preflight refusal.
        self.assertEqual(verdict.verdict_from_rc(64), verdict.Verdict.REFUSED)

    def test_rc1_truncated_still_fail(self):
        # Truncation does not change a FAIL: the fail is real regardless.
        self.assertEqual(verdict.verdict_from_rc(1, truncated=True),
                         verdict.Verdict.FAIL)

    def test_rc3_truncated_still_retry(self):
        self.assertEqual(verdict.verdict_from_rc(3, truncated=True),
                         verdict.Verdict.RETRY)

    def test_unknown_rc_is_fail(self):
        # Fail-closed: an unknown exit is never a pass (per the exit-code-trap:
        # "exit 0 is not an oracle" — and by symmetry, unknown is never pass).
        for rc in (-1, 5, 42, 127, 255):
            self.assertEqual(verdict.verdict_from_rc(rc), verdict.Verdict.FAIL,
                             "rc={} should be FAIL".format(rc))

    def test_every_verdict_value_is_distinct(self):
        # The enum must have exactly the five states the task names.
        values = {v.value for v in verdict.Verdict}
        self.assertEqual(values,
                         {"pass", "fail", "retry", "refused", "poisoned"})


class TrustworthyTest(unittest.TestCase):
    """trustworthy=False IFF verdict==pass AND truncated (#3002 shape).

    This is the precise biconditional: both directions hold.
    """

    def test_pass_not_truncated_is_trustworthy(self):
        self.assertTrue(verdict.is_trustworthy(verdict.Verdict.PASS, False))

    def test_pass_truncated_is_not_trustworthy(self):
        # The #3002 pin: a pass over a truncated run must NOT be trusted.
        self.assertFalse(verdict.is_trustworthy(verdict.Verdict.PASS, True))

    def test_fail_is_trustworthy_regardless_of_truncation(self):
        # A FAIL verdict honestly reports a failure; it is trustworthy in its
        # own assessment whether or not there was also truncation.
        self.assertTrue(verdict.is_trustworthy(verdict.Verdict.FAIL, False))
        self.assertTrue(verdict.is_trustworthy(verdict.Verdict.FAIL, True))

    def test_retry_is_trustworthy_regardless_of_truncation(self):
        self.assertTrue(verdict.is_trustworthy(verdict.Verdict.RETRY, False))
        self.assertTrue(verdict.is_trustworthy(verdict.Verdict.RETRY, True))

    def test_refused_is_trustworthy(self):
        self.assertTrue(verdict.is_trustworthy(verdict.Verdict.REFUSED, False))
        self.assertTrue(verdict.is_trustworthy(verdict.Verdict.REFUSED, True))

    def test_poisoned_is_trustworthy(self):
        # POISONED honestly reports "do not trust as a pass" — the verdict enum
        # IS the signal. `trustworthy` guards the residual risk that the verdict
        # wrongly said pass; a poisoned verdict did not say pass, so it is
        # trustworthy in its own (negative) assessment.
        self.assertTrue(verdict.is_trustworthy(verdict.Verdict.POISONED, True))
        self.assertTrue(verdict.is_trustworthy(verdict.Verdict.POISONED, False))

    def test_trustworthy_false_iff_pass_and_truncated(self):
        # The biconditional: trustworthy is False exactly for (PASS, truncated).
        for v in verdict.Verdict:
            for trunc in (True, False):
                result = verdict.is_trustworthy(v, trunc)
                expected_false = (v == verdict.Verdict.PASS and trunc)
                self.assertEqual(result, not expected_false,
                    "is_trustworthy({}, {}) = {}, expected {}".format(
                        v.value, trunc, result, not expected_false))


class DualConsumeEquivalenceTest(unittest.TestCase):
    """The 0-disagreement proof (Phase 3c): for every REACHABLE gate state, the
    verdict-based decision and the legacy $?/banner-grep decision AGREE.

    The legacy logic (gate_full_cycle.sh's original run_until_trustworthy,
    now preserved as _legacy_rc_decision) maps:
        rc==2                -> retry   (preflight refused)
        rc==3                -> retry   (coverage short)
        rc!=0 (not 2/3)      -> bail    (real failure)
        rc==0 + undercount   -> retry   (distrust, #3001)
        rc==0, clean         -> accept

    The verdict-based decision maps the Verdict enum to the same three actions:
        pass (+trustworthy)  -> accept
        refused/retry/poisoned -> retry
        fail                 -> bail

    They are derived from the SAME signals inside gate.sh (rc + undercount
    banner), so they must agree on every reachable state. The ONLY divergence is
    rc==64 (bad-mode preflight refusal): legacy bails, verdict says refused->retry.
    That state is UNREACHABLE from the cycle (gate_full_cycle passes only valid
    modes: "plain"/default and "--full"), and even if it arose, retry is harmless
    (it retries until exhaustion -> return 99, which is still non-zero, no push).
    """

    # The legacy $?/grep decision (faithful copy of _legacy_rc_decision).
    @staticmethod
    def _legacy_decision(rc, undercount):
        if rc == 2:
            return "retry"
        if rc == 3:
            return "retry"
        if rc != 0:
            return "bail"
        if undercount:
            return "retry"
        return "accept"

    # The verdict-based decision (faithful copy of run_until_trustworthy's case).
    @staticmethod
    def _verdict_decision(rc, undercount):
        truncated = undercount or (rc == 3)
        v = verdict.verdict_from_rc(rc, truncated)
        return {
            verdict.Verdict.PASS.value: "accept",
            verdict.Verdict.REFUSED.value: "retry",
            verdict.Verdict.RETRY.value: "retry",
            verdict.Verdict.POISONED.value: "retry",
            verdict.Verdict.FAIL.value: "bail",
        }[v.value]

    def test_agree_on_all_reachable_states(self):
        """For every (rc, undercount) the cycle can produce, the two decisions
        agree. The cycle's reachable exit codes are 0, 1, 2, 3 (gate.sh's exit
        taxonomy: pass/fail/preflight-refused/coverage-short). rc==64 (bad mode)
        is unreachable — the cycle passes only valid modes."""
        reachable_rcs = [0, 1, 2, 3]
        disagreements = []
        for rc in reachable_rcs:
            for undercount in (True, False):
                legacy = self._legacy_decision(rc, undercount)
                new = self._verdict_decision(rc, undercount)
                if legacy != new:
                    disagreements.append(
                        "rc={}, undercount={}: legacy={} verdict={}".format(
                            rc, undercount, legacy, new))
        self.assertEqual(disagreements, [],
            "verdict/legacy disagreement on reachable states:\n  " +
            "\n  ".join(disagreements))

    def test_rc64_divergence_is_documented_and_unreachable(self):
        """rc==64 (bad mode) is the ONLY divergence: legacy bails, verdict retries.
        This is unreachable from the cycle (valid modes only) and harmless if it
        ever arises (retry -> exhaustion -> 99, still no push). Pinned so a future
        change to either mapping notices the divergence immediately."""
        for undercount in (True, False):
            legacy = self._legacy_decision(64, undercount)
            new = self._verdict_decision(64, undercount)
            self.assertEqual(legacy, "bail")
            self.assertEqual(new, "retry")


class BuildVerdictObjTest(unittest.TestCase):
    """The schema object must carry every Plan-B field with correct types."""

    def test_has_all_top_level_fields(self):
        obj = verdict.build_verdict_obj(rc=0, truncated=False)
        for field in ("schema", "verdict", "trustworthy", "head", "mode",
                       "oracle", "started_epoch", "total_s", "failed_phase",
                       "evidence", "integrity", "runner_version"):
            self.assertIn(field, obj, "missing top-level field: {}".format(field))

    def test_evidence_has_all_sub_fields(self):
        obj = verdict.build_verdict_obj(rc=0)
        ev = obj["evidence"]
        for field in ("shard", "coverage", "asan", "arbitration"):
            self.assertIn(field, ev, "missing evidence sub-field: {}".format(field))
        for field in ("count", "reported_s"):
            self.assertIn(field, ev["shard"])
        for field in ("gtests", "littests", "expected_gtests"):
            self.assertIn(field, ev["coverage"])
        self.assertIn("errors", ev["asan"])
        self.assertIn("suspects", ev["arbitration"])

    def test_integrity_has_truncated_and_undercount_banner(self):
        obj = verdict.build_verdict_obj(rc=0, truncated=True,
                                        undercount_banner=True)
        integ = obj["integrity"]
        self.assertTrue(integ["truncated"])
        self.assertTrue(integ["undercount_banner"])

    def test_schema_constant(self):
        obj = verdict.build_verdict_obj(rc=0)
        self.assertEqual(obj["schema"], verdict.SCHEMA)

    def test_pass_obj_values(self):
        obj = verdict.build_verdict_obj(
            rc=0, truncated=False, head="abc123", mode="default",
            oracle="plain", started_epoch=1234, total_s=120.5,
            coverage_gtests=16000, expected_gtests=16000)
        self.assertEqual(obj["verdict"], "pass")
        self.assertTrue(obj["trustworthy"])
        self.assertIsNone(obj["failed_phase"])
        self.assertEqual(obj["evidence"]["coverage"]["gtests"], 16000)

    def test_poisoned_obj_values(self):
        # per #3002: rc=0 + truncated -> poisoned. trustworthy is True per the
        # iff (verdict != pass), because the poisoned verdict honestly reports
        # its outcome — it IS the distrust signal. A consumer checks the verdict
        # field, not trustworthy alone.
        obj = verdict.build_verdict_obj(rc=0, truncated=True,
                                        undercount_banner=True)
        self.assertEqual(obj["verdict"], "poisoned")
        self.assertTrue(obj["trustworthy"])

    def test_fail_with_failed_phase(self):
        obj = verdict.build_verdict_obj(rc=1, failed_phase="build")
        self.assertEqual(obj["verdict"], "fail")
        self.assertTrue(obj["trustworthy"])
        self.assertEqual(obj["failed_phase"], "build")

    def test_unmeasured_coverage_is_none_not_zero(self):
        # per #3010: an unmeasured number emits null, never 0. A harvest
        # failure and a genuine zero must stay distinguishable.
        obj = verdict.build_verdict_obj(rc=0)
        self.assertIsNone(obj["evidence"]["coverage"]["gtests"])
        self.assertIsNone(obj["evidence"]["coverage"]["expected_gtests"])
        self.assertIsNone(obj["evidence"]["asan"]["errors"])

    def test_retry_obj_values(self):
        obj = verdict.build_verdict_obj(rc=3, truncated=True,
                                        coverage_gtests=5000,
                                        expected_gtests=16000)
        self.assertEqual(obj["verdict"], "retry")
        self.assertTrue(obj["trustworthy"])


class WriteReadVerdictTest(unittest.TestCase):
    """write_verdict + read_verdict round-trip and crash invariant."""

    def test_write_then_read_round_trips(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            obj = verdict.build_verdict_obj(rc=0, head="deadbeef", mode="--full")
            verdict.write_verdict(p, obj)
            got = verdict.read_verdict(p)
            self.assertIsNotNone(got)
            self.assertEqual(got["verdict"], "pass")
            self.assertEqual(got["head"], "deadbeef")

    def test_written_file_is_valid_json(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            obj = verdict.build_verdict_obj(rc=1, failed_phase="shard")
            verdict.write_verdict(p, obj)
            with open(p) as f:
                parsed = json.load(f)  # must not raise
            self.assertEqual(parsed["verdict"], "fail")

    def test_crash_before_replace_leaves_prior_verdict_intact(self):
        # The #3002/#3003 crash invariant: a mid-exit crash (the exact scenario
        # this verdict file exists for) must leave the prior verdict intact, not
        # a partial file. Simulate by making os.replace raise.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            # Write a prior verdict.
            prior = verdict.build_verdict_obj(rc=0, head="old")
            verdict.write_verdict(p, prior)
            with open(p, "rb") as f:
                prior_bytes = f.read()

            # Simulate the crash on the next write.
            with mock.patch("gate.io.os.replace", side_effect=OSError("boom")):
                with self.assertRaises(OSError):
                    verdict.write_verdict(p, verdict.build_verdict_obj(rc=1))

            # The prior verdict is UNCHANGED (byte-for-byte), not overwritten
            # with garbage. This is what makes the file authoritative: a reader
            # polling it during/after a crash sees the last good verdict, never
            # a half-written one.
            with open(p, "rb") as f:
                self.assertEqual(f.read(), prior_bytes)
            got = verdict.read_verdict(p)
            self.assertEqual(got["head"], "old")

    def test_crash_before_replace_with_no_prior_leaves_no_file(self):
        # If there was no prior verdict, a crash must leave NO file at all —
        # not a zero-byte file, not a partial document. "No verdict written yet"
        # and "verdict written, all green" must stay distinguishable after a crash.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "fresh.json")
            with mock.patch("gate.io.os.replace", side_effect=OSError("boom")):
                with self.assertRaises(OSError):
                    verdict.write_verdict(p, verdict.build_verdict_obj(rc=0))
            self.assertFalse(os.path.exists(p))

    def test_no_orphaned_temps_after_crash(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            with mock.patch("gate.io.os.replace", side_effect=OSError("boom")):
                with self.assertRaises(OSError):
                    verdict.write_verdict(p, verdict.build_verdict_obj(rc=0))
            leftovers = [n for n in os.listdir(d) if n.endswith(".tmp")]
            self.assertEqual(leftovers, [], "orphaned temps: {}".format(leftovers))


class ReadVerdictTest(unittest.TestCase):
    """read_verdict graceful degradation: never crash on bad input."""

    def test_missing_file_returns_none(self):
        self.assertIsNone(verdict.read_verdict("/nonexistent/path/xyz.json"))

    def test_garbage_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "bad.json")
            with open(p, "w") as f:
                f.write("{not valid json")
            self.assertIsNone(verdict.read_verdict(p))

    def test_empty_file_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "empty.json")
            open(p, "w").close()
            self.assertIsNone(verdict.read_verdict(p))

    def test_non_dict_returns_none(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "list.json")
            with open(p, "w") as f:
                json.dump([1, 2, 3], f)
            self.assertIsNone(verdict.read_verdict(p))


class CliWriteTest(unittest.TestCase):
    """Smoke test the CLI gate.sh calls: `PYTHONPATH=scripts python3 -m
    gate.verdict write --path PATH --rc RC [options]` (matching the
    gate_trend.sh invocation pattern). Must produce valid JSON that
    read_verdict can parse back."""

    def _run_cli(self, args):
        scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..")
        env = dict(os.environ, PYTHONPATH=scripts_dir)
        r = subprocess.run(
            [sys.executable, "-m", "gate.verdict", "write"] + args,
            capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0,
                         "CLI failed: {} {}".format(r.stdout, r.stderr))
        return r

    def test_minimal_write_pass(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            self._run_cli(["--path", p, "--rc", "0"])
            got = verdict.read_verdict(p)
            self.assertEqual(got["verdict"], "pass")
            self.assertTrue(got["trustworthy"])

    def test_write_poisoned_via_truncated_flag(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            self._run_cli(["--path", p, "--rc", "0", "--truncated",
                           "--undercount-banner"])
            got = verdict.read_verdict(p)
            self.assertEqual(got["verdict"], "poisoned")
            self.assertTrue(got["integrity"]["undercount_banner"])

    def test_write_fail_with_context(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            self._run_cli([
                "--path", p, "--rc", "1", "--head", "abc123",
                "--mode", "default", "--failed-phase", "build",
                "--coverage-gtests", "0",
            ])
            got = verdict.read_verdict(p)
            self.assertEqual(got["verdict"], "fail")
            self.assertEqual(got["head"], "abc123")
            self.assertEqual(got["failed_phase"], "build")

    def test_write_refused_rc2(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            self._run_cli(["--path", p, "--rc", "2"])
            got = verdict.read_verdict(p)
            self.assertEqual(got["verdict"], "refused")

    def test_write_retry_rc3(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gate-verdict.json")
            self._run_cli(["--path", p, "--rc", "3", "--truncated",
                           "--coverage-gtests", "5000",
                           "--expected-gtests", "16000"])
            got = verdict.read_verdict(p)
            self.assertEqual(got["verdict"], "retry")
            self.assertTrue(got["trustworthy"])
            self.assertTrue(got["integrity"]["truncated"])

    def test_no_subcommand_exits_nonzero(self):
        scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..")
        env = dict(os.environ, PYTHONPATH=scripts_dir)
        r = subprocess.run(
            [sys.executable, "-m", "gate.verdict"],
            capture_output=True, text=True, env=env)
        self.assertNotEqual(r.returncode, 0)


class CliReadTest(unittest.TestCase):
    """Tests for the `read` subcommand — the consumer-side entry point (Phase
    3c, #3002). Exit 0 iff pass && trustworthy; 1 iff present-not-pass; 2 iff
    missing/unparseable/stale/missing-field."""

    def _run_cli(self, args):
        scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..")
        env = dict(os.environ, PYTHONPATH=scripts_dir)
        r = subprocess.run(
            [sys.executable, "-m", "gate.verdict", "read"] + args,
            capture_output=True, text=True, env=env)
        return r

    def _write(self, d, rc=0, **kw):
        p = os.path.join(d, "gate-verdict.json")
        obj = verdict.build_verdict_obj(rc=rc, **kw)
        verdict.write_verdict(p, obj)
        return p

    def test_pass_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=0)
            r = self._run_cli(["--path", p])
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_fail_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=1)
            r = self._run_cli(["--path", p])
            self.assertEqual(r.returncode, 1)

    def test_poisoned_exits_one(self):
        # poisoned is not a trustworthy pass -> exit 1 (consumer maps to retry).
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=0, truncated=True, undercount_banner=True)
            r = self._run_cli(["--path", p])
            self.assertEqual(r.returncode, 1)

    def test_retry_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=3)
            r = self._run_cli(["--path", p])
            self.assertEqual(r.returncode, 1)

    def test_refused_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=2)
            r = self._run_cli(["--path", p])
            self.assertEqual(r.returncode, 1)

    def test_missing_file_exits_two(self):
        # A consumer must fall back to $?/greps when the verdict is absent
        # (back-compat; the writer is best-effort).
        r = self._run_cli(["--path", "/nonexistent/verdict-xyz-12345.json"])
        self.assertEqual(r.returncode, 2)

    def test_garbage_exits_two(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "bad.json")
            with open(p, "w") as f:
                f.write("{not json")
            r = self._run_cli(["--path", p])
            self.assertEqual(r.returncode, 2)

    def test_field_verdict_prints_value(self):
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=3)
            r = self._run_cli(["--path", p, "--field", "verdict"])
            self.assertEqual(r.stdout.strip(), "retry")
            # exit code is still the pass-predicate, not the field.
            self.assertEqual(r.returncode, 1)

    def test_field_trustworthy_prints_lowercase(self):
        # Booleans print as JSON lowercases so a shell test [ "$v" = true ] works.
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=0)
            r = self._run_cli(["--path", p, "--field", "trustworthy"])
            self.assertEqual(r.stdout.strip(), "true")
            self.assertEqual(r.returncode, 0)

    def test_field_does_not_change_exit_code(self):
        # --field verdict on a pass still exits 0 (the field is display-only).
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=0, head="abc123")
            r = self._run_cli(["--path", p, "--field", "head"])
            self.assertEqual(r.stdout.strip(), "abc123")
            self.assertEqual(r.returncode, 0)

    def test_field_null_prints_null(self):
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=0)  # failed_phase defaults to None
            r = self._run_cli(["--path", p, "--field", "failed_phase"])
            self.assertEqual(r.stdout.strip(), "null")

    def test_missing_field_exits_two(self):
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=0)
            r = self._run_cli(["--path", p, "--field", "nonexistent_field"])
            self.assertEqual(r.returncode, 2)

    def test_expect_head_match_exits_normally(self):
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=0, head="deadbeef")
            r = self._run_cli(["--path", p, "--expect-head", "deadbeef"])
            self.assertEqual(r.returncode, 0)

    def test_expect_head_mismatch_exits_two(self):
        # The disambiguation a poller uses to reject a stale/concurrent gate's
        # verdict on the shared default path (#3000 shape).
        with tempfile.TemporaryDirectory() as d:
            p = self._write(d, rc=0, head="aaaa1111")
            r = self._run_cli(["--path", p, "--expect-head", "bbbb2222"])
            self.assertEqual(r.returncode, 2)
            self.assertIn("head mismatch", r.stderr)

    def test_read_then_write_round_trips_via_cli(self):
        # End-to-end: write via CLI, read via CLI — the exact pattern the
        # consumer scripts use.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gv.json")
            scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..")
            env = dict(os.environ, PYTHONPATH=scripts_dir)
            subprocess.run(
                [sys.executable, "-m", "gate.verdict", "write",
                 "--path", p, "--rc", "0", "--head", "cafef00d",
                 "--mode=--full", "--oracle", "asan"],
                capture_output=True, text=True, env=env, check=True)
            r = subprocess.run(
                [sys.executable, "-m", "gate.verdict", "read",
                 "--path", p, "--field", "oracle", "--expect-head", "cafef00d"],
                capture_output=True, text=True, env=env)
            self.assertEqual(r.stdout.strip(), "asan")
            self.assertEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
