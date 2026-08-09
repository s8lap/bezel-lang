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


class DetectTruncationTest(unittest.TestCase):
    """detect_truncation owns the banner grep that lived in gate.sh (#3010 W3).

    Pins: (a) the literal banner in a readable log -> undercount_banner=True;
    (b) rc==3 -> truncated even with no banner; (c) a missing/unreadable shard
    log -> no banner seen (mirrors bash's `[ -r ... ] &&` readability guard);
    (d) the composite `truncated` = undercount_banner OR rc==3.
    """

    def test_clean_log_rc0_is_not_truncated(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "shard.log")
            with open(log, "w") as f:
                f.write("## GATE PASS — all shards reported\n")
            trunc, ubanner = verdict.detect_truncation(log, 0)
        self.assertFalse(trunc)
        self.assertFalse(ubanner)

    def test_banner_in_log_sets_undercount_banner(self):
        # The exact run_parallel.sh:300 wording — detect_truncation must spot the
        # "shard crash / undercount" substring (not the full banner line).
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "shard.log")
            with open(log, "w") as f:
                f.write("...\n## GATE FAILURE: shard crash / undercount — "
                        "result is NOT trustworthy\n...\n")
            trunc, ubanner = verdict.detect_truncation(log, 0)
        self.assertTrue(ubanner)
        # rc=0 + banner -> POISONED: truncated must be True so verdict_from_rc
        # flips the pass to poisoned (#3002 shape).
        self.assertTrue(trunc)

    def test_rc3_is_truncated_even_without_banner(self):
        # The coverage-short signal (rc==3) sets truncated independently of the
        # banner — gate_full_cycle retries on exit 3 (#3002).
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "shard.log")
            with open(log, "w") as f:
                f.write("clean log, no banner\n")
            trunc, ubanner = verdict.detect_truncation(log, 3)
        self.assertTrue(trunc)
        self.assertFalse(ubanner)  # the banner itself was absent

    def test_missing_log_is_not_truncated(self):
        # No shard log (early preflight exit) -> no banner -> not truncated
        # (mirrors bash `[ -r "$S/gate_shard.log" ] && grep ...`).
        trunc, ubanner = verdict.detect_truncation("/nonexistent/shard.log", 0)
        self.assertFalse(trunc)
        self.assertFalse(ubanner)

    def test_none_log_is_not_truncated(self):
        trunc, ubanner = verdict.detect_truncation(None, 0)
        self.assertFalse(trunc)
        self.assertFalse(ubanner)

    def test_unreadable_log_is_not_truncated(self):
        # A path that exists but cannot be opened -> no banner seen (the OSError
        # is swallowed, matching grep's 2>/dev/null).
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "noperm.log")
            with open(log, "w") as f:
                f.write("## GATE FAILURE: shard crash / undercount\n")
            os.chmod(log, 0o000)
            try:
                trunc, ubanner = verdict.detect_truncation(log, 0)
                # If running as root, chmod 0o000 is bypassed — skip the assert.
                if os.geteuid() != 0:
                    self.assertFalse(trunc)
                    self.assertFalse(ubanner)
            finally:
                os.chmod(log, 0o600)

    def test_rc3_with_banner_keeps_both_signals(self):
        with tempfile.TemporaryDirectory() as d:
            log = os.path.join(d, "shard.log")
            with open(log, "w") as f:
                f.write("shard crash / undercount\n")
            trunc, ubanner = verdict.detect_truncation(log, 3)
        self.assertTrue(trunc)
        self.assertTrue(ubanner)

    def test_banner_literal_constant(self):
        # Pinned: the wording run_parallel.sh emits, verbatim. A drift here is
        # the silent-undetected-truncation defect (the grep re-implementation
        # the Wave-3 migration consolidated into ONE owner).
        self.assertEqual(verdict.UNDERCOUNT_BANNER, "shard crash / undercount")


class OptionalIntTest(unittest.TestCase):
    """_optional_int is the move that collapses the 12 conditional arg guards:
    gate.sh passes ${VAR:-} unconditionally and Python owns set-vs-unset.
    """

    def test_empty_is_none(self):
        # Unset (-> "") emits null, never 0 (per #3010).
        self.assertIsNone(verdict._optional_int(""))
        self.assertIsNone(verdict._optional_int(None))

    def test_zero_is_zero(self):
        # A SET 0 stays 0 — distinguishable from unmeasured (null).
        self.assertEqual(verdict._optional_int("0"), 0)

    def test_count_is_int(self):
        self.assertEqual(verdict._optional_int("16482"), 16482)


class CliWriteFromStateTest(unittest.TestCase):
    """Smoke test the write_from_state CLI gate.sh now calls (Wave 3)."""

    def _run_cli(self, args):
        scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..")
        env = dict(os.environ, PYTHONPATH=scripts_dir)
        r = subprocess.run(
            [sys.executable, "-m", "gate.verdict", "write_from_state"] + args,
            capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0,
                         "CLI failed: {} {}".format(r.stdout, r.stderr))
        return r

    def test_minimal_pass(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gv.json")
            log = os.path.join(d, "shard.log")
            with open(log, "w") as f:
                f.write("clean\n")
            self._run_cli(["--path", p, "--rc", "0", "--shard-log", log])
            got = verdict.read_verdict(p)
        self.assertEqual(got["verdict"], "pass")
        self.assertTrue(got["trustworthy"])
        self.assertFalse(got["integrity"]["truncated"])

    def test_poisoned_via_shard_log_banner(self):
        # The truncation detection moved INTO Python: write_from_state reads the
        # shard log itself, so a banner in the log flips rc=0 to poisoned WITHOUT
        # the caller passing --truncated/--undercount-banner.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gv.json")
            log = os.path.join(d, "shard.log")
            with open(log, "w") as f:
                f.write("## GATE FAILURE: shard crash / undercount\n")
            self._run_cli(["--path", p, "--rc", "0", "--shard-log", log])
            got = verdict.read_verdict(p)
        self.assertEqual(got["verdict"], "poisoned")
        self.assertTrue(got["integrity"]["truncated"])
        self.assertTrue(got["integrity"]["undercount_banner"])

    def test_unmeasured_coverage_is_null(self):
        # The #3010 pin preserved through the migration: an empty coverage field
        # (gate.sh's ${COV_GTESTS:-} on an early exit) emits null, never 0.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gv.json")
            self._run_cli(["--path", p, "--rc", "2", "--coverage-gtests", ""])
            got = verdict.read_verdict(p)
        self.assertIsNone(got["evidence"]["coverage"]["gtests"])

    def test_measured_zero_coverage_is_zero(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gv.json")
            self._run_cli(["--path", p, "--rc", "0", "--coverage-gtests", "0"])
            got = verdict.read_verdict(p)
        self.assertEqual(got["evidence"]["coverage"]["gtests"], 0)

    def test_mode_full_via_equals_form(self):
        # MODE can be '--full'; the caller uses --mode=VALUE so argparse does not
        # mistake the value for an unknown flag (the #3010 comment preserved).
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "gv.json")
            self._run_cli(["--path", p, "--rc", "0", "--mode=--full",
                           "--oracle", "asan"])
            got = verdict.read_verdict(p)
        self.assertEqual(got["mode"], "--full")
        self.assertEqual(got["oracle"], "asan")


class DualRunWriteFromStateTest(unittest.TestCase):
    """The dual-run equivalence pin (#3010 Wave 3): the verdict file written by
    the new `write_from_state` (Python owns truncation detection + arg assembly)
    MUST equal the file the old `write` + bash arg-assembly produced, over every
    reachable gate state. This is the load-bearing proof that the migration
    changed WHERE the logic lives without changing WHAT it computes.

    The old path is reproduced faithfully (_old_assemble — a Python copy of
    gate.sh's pre-Wave-3 gate_write_verdict: the bash banner grep + the 12
    conditional `[ -n "${X+x}" ]` / `[ "${X:-0}" -gt 0 ]` guards, feeding the
    unchanged low-level `write` subcommand). Both paths run as real CLIs on the
    same state; the two JSON files must be equal.
    """

    @staticmethod
    def _oracle(state):
        return "asan" if state.get("ASAN") == "1" else "plain"

    @staticmethod
    def _old_assemble(state, path):
        """Faithful replica of gate.sh's pre-Wave-3 gate_write_verdict arg
        assembly (the truncation grep + 12 conditional guards). Returns argv for
        `gate.verdict write`. The dual-run oracle."""
        rc = state["rc"]
        shard_log = state.get("shard_log")
        # bash: `if [ -r "$S/gate_shard.log" ] && grep -q '...'; then trunc=1; ubanner=1; fi`
        trunc = 0
        ubanner = 0
        if shard_log and os.path.isfile(shard_log):
            try:
                with open(shard_log, errors="replace") as f:
                    if "shard crash / undercount" in f.read():
                        trunc = 1
                        ubanner = 1
            except OSError:
                pass
        # bash: `[ "$rc" -eq 3 ] && trunc=1`
        if rc == 3:
            trunc = 1
        args = ["--path", path, "--rc", str(rc),
                "--head", state.get("GATE_HEAD", "") or "",
                "--mode=" + (state.get("MODE", "") or ""),
                "--oracle", DualRunWriteFromStateTest._oracle(state),
                "--started-epoch", str(state.get("GATE_EPOCH", 0) or 0),
                "--total-s", str(state.get("total", 0) or 0)]
        # bash: `local _fp="${FAILED_PHASE:-}"; [ -n "$_fp" ] && vargs+=(...)`
        if state.get("FAILED_PHASE"):
            args += ["--failed-phase", state["FAILED_PHASE"]]
        if trunc:
            args += ["--truncated"]
        if ubanner:
            args += ["--undercount-banner"]
        # bash: `[ "${SHARD_COUNT:-0}" -gt 0 ] 2>/dev/null && vargs+=(...)`
        sc = state.get("SHARD_COUNT") or 0
        if sc > 0:
            args += ["--shard-count", str(sc)]
        sr = state.get("SHARD_REPORTED_S") or 0
        if sr > 0:
            args += ["--shard-reported-s", str(sr)]
        # bash: `[ -n "${X+x}" ] && vargs+=(...)` — pass iff SET (not None).
        for flag, key in (("--coverage-gtests", "COV_GTESTS"),
                          ("--coverage-littests", "COV_LITTESTS"),
                          ("--expected-gtests", "GATE_EXPECTED_GTESTS"),
                          ("--asan-errors", "ASAN_ERRS"),
                          ("--arbitration-suspects", "ARB_SUSPECTS")):
            v = state.get(key)
            if v is not None:
                args += [flag, str(v)]
        return args

    @staticmethod
    def _new_assemble(state, path):
        """The new write_from_state argv — gate.sh's post-Wave-3 call: all
        fields passed unconditionally with ${VAR:-} defaults; Python owns the
        truncation detection (via --shard-log) and set-vs-unset normalization
        (empty coverage -> null)."""
        s = lambda k, default="": state.get(k, default) or default
        cov = lambda k: ("" if state.get(k) is None else str(state.get(k)))
        return ["--path", path, "--rc", str(state["rc"]),
                "--shard-log", state.get("shard_log") or "",
                "--head", s("GATE_HEAD"),
                "--mode=" + s("MODE"),
                "--oracle", DualRunWriteFromStateTest._oracle(state),
                "--started-epoch", str(state.get("GATE_EPOCH", 0) or 0),
                "--total-s", str(state.get("total", 0) or 0),
                "--failed-phase", s("FAILED_PHASE"),
                "--shard-count", str(state.get("SHARD_COUNT") or 0),
                "--shard-reported-s", str(state.get("SHARD_REPORTED_S") or 0),
                "--coverage-gtests", cov("COV_GTESTS"),
                "--coverage-littests", cov("COV_LITTESTS"),
                "--expected-gtests", cov("GATE_EXPECTED_GTESTS"),
                "--asan-errors", cov("ASAN_ERRS"),
                "--arbitration-suspects", cov("ARB_SUSPECTS")]

    def _run_module(self, subcmd, args):
        scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..")
        env = dict(os.environ, PYTHONPATH=scripts_dir)
        r = subprocess.run(
            [sys.executable, "-m", "gate.verdict", subcmd] + args,
            capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0,
                         "{} failed: {} {}".format(subcmd, r.stdout, r.stderr))

    def _states(self, shard_dir):
        """A matrix of reachable gate states (the exit-code taxonomy 0/1/2/3/64
        x banner-present/absent/early x coverage measured/unmeasured x plain/
        asan). Each exercises a distinct branch of the old arg-assembly."""
        clean = os.path.join(shard_dir, "clean.log")
        with open(clean, "w") as f:
            f.write("## GATE PASS — 16 shards, all summaries present\n")
        banner = os.path.join(shard_dir, "banner.log")
        with open(banner, "w") as f:
            f.write("...\n## GATE FAILURE: shard crash / undercount — x\n")
        return [
            # rc=0 clean pass, full harvest (the happy path).
            {"rc": 0, "shard_log": clean, "GATE_HEAD": "abc123",
             "MODE": "default", "GATE_EPOCH": 1234, "total": 120,
             "FAILED_PHASE": None, "SHARD_COUNT": 16, "SHARD_REPORTED_S": 400,
             "COV_GTESTS": 16482, "COV_LITTESTS": 2103,
             "GATE_EXPECTED_GTESTS": 16482, "ASAN_ERRS": 0, "ARB_SUSPECTS": 0,
             "ASAN": "0"},
            # rc=0 + undercount banner -> POISONED (the #3002 shape).
            {"rc": 0, "shard_log": banner, "GATE_HEAD": "abc123",
             "MODE": "default", "total": 90, "SHARD_COUNT": 16,
             "COV_GTESTS": 4000, "GATE_EXPECTED_GTESTS": 16482, "ASAN": "0"},
            # rc=1 real failure with a failed phase + asan errors + arb suspects.
            {"rc": 1, "shard_log": clean, "FAILED_PHASE": "build",
             "MODE": "--full", "ASAN": "1", "SHARD_COUNT": 8,
             "COV_GTESTS": 0, "COV_LITTESTS": 0, "GATE_EXPECTED_GTESTS": 16482,
             "ASAN_ERRS": 3, "ARB_SUSPECTS": 2},
            # rc=2 preflight refused — EARLY exit: harvest vars UNSET (None),
            # no shard log. Coverage must be null, never 0 (#3010).
            {"rc": 2, "shard_log": None, "GATE_HEAD": None, "MODE": "default",
             "FAILED_PHASE": None, "SHARD_COUNT": None, "COV_GTESTS": None,
             "COV_LITTESTS": None, "GATE_EXPECTED_GTESTS": None,
             "ASAN_ERRS": None, "ARB_SUSPECTS": None, "ASAN": "0"},
            # rc=3 coverage short (#3002) — truncated via rc, FAILED_PHASE set.
            {"rc": 3, "shard_log": clean, "FAILED_PHASE": "coverage floor",
             "MODE": "default", "SHARD_COUNT": 16, "COV_GTESTS": 5000,
             "GATE_EXPECTED_GTESTS": 16482, "ASAN": "0"},
            # rc=64 unrecognized mode — EARLY exit (harvest unset), MODE=default.
            {"rc": 64, "shard_log": None, "MODE": "default", "ASAN": "0"},
            # rc=0 pass with coverage explicitly measured as 0 (the 0-vs-null
            # boundary: SET 0 must stay 0, not collapse to null).
            {"rc": 0, "shard_log": clean, "MODE": "default", "SHARD_COUNT": 0,
             "COV_GTESTS": 0, "COV_LITTESTS": 0, "GATE_EXPECTED_GTESTS": 16482,
             "ASAN": "0"},
            # rc=0 asan pass, mode=--full, GATE_HEAD unset (early-ish).
            {"rc": 0, "shard_log": clean, "MODE": "--full", "ASAN": "1",
             "COV_GTESTS": 16482, "GATE_EXPECTED_GTESTS": 16482,
             "ASAN_ERRS": 0},
        ]

    def test_new_verdict_equals_old_over_all_reachable_states(self):
        """For every state in the matrix, the verdict file from write_from_state
        equals the file from the old write + bash arg-assembly. This is the
        dual-run proof the Wave-3 migration requires: same verdict, same
        trustworthy, same truncated, same oracle, same evidence/integrity."""
        with tempfile.TemporaryDirectory() as d:
            shard_dir = os.path.join(d, "shards")
            os.makedirs(shard_dir)
            for i, state in enumerate(self._states(shard_dir)):
                old_p = os.path.join(d, "old{}.json".format(i))
                new_p = os.path.join(d, "new{}.json".format(i))
                self._run_module("write", self._old_assemble(state, old_p))
                self._run_module("write_from_state",
                                 self._new_assemble(state, new_p))
                old = verdict.read_verdict(old_p)
                new = verdict.read_verdict(new_p)
                self.assertIsNotNone(old, "old path wrote nothing for state {}".format(i))
                self.assertIsNotNone(new, "new path wrote nothing for state {}".format(i))
                self.assertEqual(old, new,
                    "state #{} (rc={}): new write_from_state verdict differs "
                    "from old write verdict.\nOLD: {}\nNEW: {}".format(
                        i, state["rc"],
                        json.dumps(old, sort_keys=True),
                        json.dumps(new, sort_keys=True)))


if __name__ == "__main__":
    unittest.main()
