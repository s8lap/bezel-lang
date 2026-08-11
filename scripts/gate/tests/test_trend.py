"""Tests for gate.trend (#3010, Phase 1).

The five rules (R1-R5) are a PRESERVATION CONTRACT (Plan-A 1.16/1.17/1.18),
lifted verbatim from the gate_trend.sh heredoc. These tests pin each rule's
fire/no-fire behavior on hand-built records so a faithful port stays faithful:
one unit test per rule, plus the malformed-newest refusal (Plan-A 1.17), the
high-water proxy for legacy schema:1 records, the --last window, and the
unreadable-log / unknown-arg exit codes. The byte-identical equivalence over
the 465+ real records lives in the Phase-1 commit message; these tests are the
fast, localized guard against a future drift.
"""

import contextlib
import io as _stdio_io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import trend  # noqa: E402


def _scan(judge_result):
    """Pull the scan_line out of judge()'s (scan_line, findings) return."""
    return judge_result[0]


def _findings(judge_result):
    return judge_result[1]


def _fired_rules(findings):
    return {f[1] for f in findings}


def _rec(**kw):
    """A minimal record with sensible defaults so tests read at a glance."""
    base = {
        "ts": "2026-08-08T00:00:00Z",
        "head_short": "abc1234",
        "mode": "--full",
        "result": "pass",
    }
    base.update(kw)
    return base


class R1CoverageShortButPassTest(unittest.TestCase):
    def test_pass_below_99pct_of_expected_fires_fail(self):
        rec = _rec(expected_gtests=1000, gtests=500)
        scan, findings = trend.judge([rec])
        self.assertIn("1 records scanned, 5 rules evaluated", scan)
        rules = _fired_rules(findings)
        self.assertIn(trend.RULES[0], rules)
        sev = [f[0] for f in findings if f[1] == trend.RULES[0]]
        self.assertEqual(sev, ["FAIL"])
        self.assertIn("only 500 of 1000 gtests ran (50.0%)", findings[0][3])

    def test_pass_at_full_coverage_does_not_fire(self):
        rec = _rec(expected_gtests=1000, gtests=1000)
        _, findings = trend.judge([rec])
        self.assertNotIn(trend.RULES[0], _fired_rules(findings))

    def test_fail_verdict_is_not_judged_for_coverage(self):
        # R1 is a contradiction in a PASS; a failing gate's coverage is its own
        # business and must not carry a second, spurious finding.
        rec = _rec(result="fail", expected_gtests=1000, gtests=0)
        _, findings = trend.judge([rec])
        self.assertNotIn(trend.RULES[0], _fired_rules(findings))

    def test_pass_at_99pct_boundary_does_not_fire(self):
        # 99% exactly is the boundary (got < want*0.99 is strict) -> clean.
        rec = _rec(expected_gtests=1000, gtests=990)
        _, findings = trend.judge([rec])
        self.assertNotIn(trend.RULES[0], _fired_rules(findings))


class R2BinaryContributedZeroTest(unittest.TestCase):
    def test_binary_with_expected_but_ran_zero_fires_fail(self):
        rec = _rec(
            expected_gtests=10, gtests=10,
            binaries={"codegen": {"ran": 0, "expected": 200},
                      "sema": {"ran": 10, "expected": 10}})
        _, findings = trend.judge([rec])
        self.assertIn(trend.RULES[1], _fired_rules(findings))
        msg = [f[3] for f in findings if f[1] == trend.RULES[1]][0]
        self.assertIn("binary 'codegen' ran 0 of 200 tests", msg)
        self.assertEqual([f[0] for f in findings if f[1] == trend.RULES[1]], ["FAIL"])

    def test_binary_short_of_expected_warns_only(self):
        rec = _rec(
            expected_gtests=10, gtests=9,
            binaries={"codegen": {"ran": 150, "expected": 200}})
        _, findings = trend.judge([rec])
        self.assertIn(trend.RULES[1], _fired_rules(findings))
        self.assertEqual([f[0] for f in findings if f[1] == trend.RULES[1]], ["WARN"])

    def test_binary_at_full_does_not_fire(self):
        rec = _rec(
            expected_gtests=10, gtests=10,
            binaries={"codegen": {"ran": 200, "expected": 200}})
        _, findings = trend.judge([rec])
        self.assertNotIn(trend.RULES[1], _fired_rules(findings))


class R3WarmSlowerThanColdTest(unittest.TestCase):
    def test_warm_slower_than_worst_recent_cold_fires_fail(self):
        cold = _rec(cold=True, shard_reported_s=100)
        warm = _rec(cold=False, shard_reported_s=200)
        _, findings = trend.judge([cold, warm])
        self.assertIn(trend.RULES[2], _fired_rules(findings))
        msg = [f[3] for f in findings if f[1] == trend.RULES[2]][0]
        self.assertIn("WARM shard pass took 200s", msg)
        self.assertIn("100s)", msg)

    def test_warm_not_slower_than_cold_does_not_fire(self):
        cold = _rec(cold=True, shard_reported_s=200)
        warm = _rec(cold=False, shard_reported_s=100)
        _, findings = trend.judge([cold, warm])
        self.assertNotIn(trend.RULES[2], _fired_rules(findings))

    def test_cold_history_is_per_mode(self):
        # A cold run in one mode must not seed the baseline of another mode.
        cold_full = _rec(cold=True, mode="--full", shard_reported_s=100)
        warm_quick = _rec(cold=False, mode="--quick", shard_reported_s=200)
        _, findings = trend.judge([cold_full, warm_quick])
        self.assertNotIn(trend.RULES[2], _fired_rules(findings))


class R4UnmeasuredSentinelTest(unittest.TestCase):
    def _shard(self, s=60):
        return [{"name": "shard", "s": s, "rc": 0, "status": "ran", "note": ""}]

    def test_pass_with_ran_shard_but_gtests_zero_fires_fail(self):
        rec = _rec(expected_gtests=100, gtests=0, phases=self._shard(60))
        _, findings = trend.judge([rec])
        self.assertIn(trend.RULES[3], _fired_rules(findings))
        msg = [f[3] for f in findings if f[1] == trend.RULES[3] and f[0] == "FAIL"][0]
        self.assertIn("shard phase ran 60s but gtests is 0", msg)

    def test_fail_verdict_does_not_trigger_r4(self):
        # R4 is scoped to PASS: a gate that failed early legitimately never
        # reached the coverage harvest.
        rec = _rec(result="fail", gtests=0, phases=self._shard(60))
        _, findings = trend.judge([rec])
        self.assertNotIn(trend.RULES[3], _fired_rules(findings))

    def test_shard_under_30s_does_not_trigger_r4(self):
        rec = _rec(expected_gtests=100, gtests=0, phases=self._shard(29))
        _, findings = trend.judge([rec])
        self.assertNotIn(trend.RULES[3], _fired_rules(findings))

    def test_cache_n_zero_on_warm_run_warns(self):
        rec = _rec(expected_gtests=100, gtests=100, cold=False,
                   cache_n=0, phases=self._shard(60))
        _, findings = trend.judge([rec])
        warns = [f for f in findings if f[1] == trend.RULES[3] and f[0] == "WARN"]
        self.assertTrue(any("cache_n=0" in f[3] for f in warns))


class R5CoverageFloorDisabledTest(unittest.TestCase):
    def test_floor_false_fires_fail(self):
        rec = _rec(expected_gtests=100, gtests=100, coverage_floor_enforced=False)
        _, findings = trend.judge([rec])
        self.assertIn(trend.RULES[4], _fired_rules(findings))
        self.assertEqual([f[0] for f in findings if f[1] == trend.RULES[4]], ["FAIL"])

    def test_floor_true_does_not_fire(self):
        rec = _rec(expected_gtests=100, gtests=100, coverage_floor_enforced=True)
        _, findings = trend.judge([rec])
        self.assertNotIn(trend.RULES[4], _fired_rules(findings))

    def test_floor_absent_does_not_fire(self):
        # Schema:1 records predate the floor field; absence is not False.
        rec = _rec(expected_gtests=100, gtests=100)
        _, findings = trend.judge([rec])
        self.assertNotIn(trend.RULES[4], _fired_rules(findings))


class HighWaterProxyTest(unittest.TestCase):
    """Schema:1 records carry no expected_gtests; R1 falls back to the running
    high-water of gtests as the expected-count proxy (#3002 landed 2026-08-07)."""

    def test_legacy_pass_below_high_water_fires_with_proxy_message(self):
        old = _rec(gtests=10000)            # no expected_gtests -> schema:1
        new = _rec(gtests=5000)             # well below the high-water it set
        _, findings = trend.judge([old, new])
        self.assertIn(trend.RULES[0], _fired_rules(findings))
        msg = [f[3] for f in findings if f[1] == trend.RULES[0]][0]
        self.assertIn("high-water proxy", msg)
        self.assertIn("5000 of 10000", msg)

    def test_legacy_record_alone_with_zero_gtests_does_not_fire(self):
        # With no prior high-water, want is 0 (falsy) -> R1 cannot fire. This is
        # the faithful heredoc behavior, not a bug to fix.
        rec = _rec(gtests=0)
        _, findings = trend.judge([rec])
        self.assertNotIn(trend.RULES[0], _fired_rules(findings))


class OnlyLastWindowTest(unittest.TestCase):
    def test_last_window_judges_only_the_newest_record(self):
        # The middle record has the floor disabled; under --last only the
        # (clean) newest record is judged, so R5 stays silent. Full scan fires.
        clean0 = _rec(expected_gtests=10, gtests=10, ts="t0")
        bad = _rec(expected_gtests=10, gtests=10, ts="t1",
                   coverage_floor_enforced=False)
        clean2 = _rec(expected_gtests=10, gtests=10, ts="t2")
        records = [clean0, bad, clean2]

        _, last_findings = trend.judge(records, only_last=True)
        self.assertNotIn(trend.RULES[4], _fired_rules(last_findings))

        _, full_findings = trend.judge(records, only_last=False)
        self.assertIn(trend.RULES[4], _fired_rules(full_findings))

    def test_last_window_empty_cold_history_so_r3_cannot_fire(self):
        # Faithful to the heredoc: cold_hist is built inside the judge loop, so
        # under --last the single visited record sees an empty history and R3
        # is structurally inert (a rule never uses information from the future).
        cold = _rec(cold=True, shard_reported_s=100, ts="t0")
        warm = _rec(cold=False, shard_reported_s=2000, ts="t1")
        records = [cold, warm]
        _, findings = trend.judge(records, only_last=True)
        self.assertNotIn(trend.RULES[2], _fired_rules(findings))
        # ... but a full scan of the same two records DOES fire R3.
        _, full = trend.judge(records, only_last=False)
        self.assertIn(trend.RULES[2], _fired_rules(full))


class MalformedNewestRefusalTest(unittest.TestCase):
    """Plan-A 1.17: a malformed NEWEST line is a loud FAIL, never a silent
    fallback to grading an older record."""

    def _write_log(self, lines):
        fd, path = tempfile.mkstemp(suffix=".jsonl")
        os.close(fd)
        with open(path, "w") as f:
            for ln in lines:
                f.write(ln)
                f.write("\n")
        return path

    def test_malformed_last_line_refuses_loudly(self):
        path = self._write_log([
            json.dumps(_rec(expected_gtests=10, gtests=10, ts="t0")),
            "{this is not json",
        ])
        try:
            buf = _stdio_io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = trend.main(["--log", path, "--last"])
            out = buf.getvalue()
            self.assertEqual(rc, 1)
            self.assertIn("0 rules evaluated", out)
            self.assertIn("NEWEST record is malformed JSON", out)
            self.assertIn("Refusing to silently grade an older record", out)
        finally:
            os.unlink(path)

    def test_parseable_log_does_not_refuse(self):
        path = self._write_log([
            json.dumps(_rec(expected_gtests=10, gtests=10, ts="t0")),
            json.dumps(_rec(expected_gtests=10, gtests=10, ts="t1")),
        ])
        try:
            buf = _stdio_io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = trend.main(["--log", path, "--last"])
            self.assertEqual(rc, 0)
            self.assertIn("2 records scanned", buf.getvalue())
        finally:
            os.unlink(path)


class ExitCodesTest(unittest.TestCase):
    def test_unreadable_log_returns_2(self):
        buf = _stdio_io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = trend.main(["--log", "/nonexistent/path/to/timings.jsonl"])
        self.assertEqual(rc, 2)
        out = buf.getvalue()
        self.assertIn("CANNOT READ", out)
        self.assertIn("0 records scanned, 0 rules evaluated", out)

    def test_unknown_argument_returns_64(self):
        err = _stdio_io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = trend.main(["--bogus"])
        self.assertEqual(rc, 64)
        self.assertIn("unknown argument", err.getvalue())

    def test_clean_run_returns_0_and_prints_no_rule_fired(self):
        rec = _rec(expected_gtests=100, gtests=100, coverage_floor_enforced=True,
                   phases=[{"name": "shard", "s": 5, "status": "ran"}])
        path = tempfile.mkstemp(suffix=".jsonl")[1]
        try:
            with open(path, "w") as f:
                f.write(json.dumps(rec))
                f.write("\n")
            buf = _stdio_io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = trend.main(["--log", path])
            self.assertEqual(rc, 0)
            self.assertIn("no rule fired", buf.getvalue())
        finally:
            os.unlink(path)


class ScanLineContractTest(unittest.TestCase):
    """The scan line is UNCONDITIONAL and is the caller's proof this ran. A
    missing scan line is a failure, not a pass (Plan-A 1.17)."""

    def test_scan_line_always_returned_with_rule_count(self):
        scan, _ = trend.judge([])
        self.assertIn("0 records scanned, 5 rules evaluated", scan)

    def test_malformed_count_appears_in_scan_suffix(self):
        # When malformed lines were seen (but the newest was parseable), the
        # scan line carries a ", N malformed lines skipped" suffix.
        rec = _rec(expected_gtests=10, gtests=10)
        scan, _ = trend.judge([rec], malformed=3)
        self.assertIn("3 malformed lines skipped", scan)


if __name__ == "__main__":
    unittest.main()
