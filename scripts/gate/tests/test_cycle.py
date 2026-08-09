"""Tests for gate.cycle (#3010 Wave 3 — the shared gate-cycle classifier).

The four load-bearing pin groups the task requires:
  1. classify — the verdict->decision truth table over all 5 verdicts x
     trustworthy. The load-bearing cell is (pass, False) -> retry: a pass the
     writer somehow let through over a truncated run is NEVER accepted.
  2. cross_check — conservative-non-accept over ALL 3x3 decision pairs (the
     policy that stopped the 55-truncated-gates-over-24-days class).
  3. assert_asan_ran — the oracle-verdict + log-grep evidence check over the
     evidence combinations.
  4. The 0-disagreement proof — classify(verdict_from_rc(rc)) == legacy_decision
     (rc) on every reachable gate state (rc in {0,1,2,3} x truncation), so the
     two derivations cross_check reconciles never disagree on a reachable state.

Plus CLI smoke tests for each subcommand (the runner scripts' entry points) and
the dual-consume equivalence pin: the CLI decision word is byte-identical to the
legacy bash `echo accept|retry|bail` on every state.
"""

import collections
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import cycle  # noqa: E402
from gate import verdict  # noqa: E402


# ---------------------------------------------------------------------------
# 1. classify — the verdict->decision truth table.
# ---------------------------------------------------------------------------

class ClassifyTest(unittest.TestCase):
    """classify: the verdict-path decision (5 verdicts x trustworthy {T,F}).

    The contract mirrors gate_full_cycle.sh's run_until_trustworthy verdict case
    (:136-143) plus the defense-in-depth trustworthy check (the integration
    test's ConsumerDecisionMappingTest pin): accept ONLY on pass+trustworthy;
    a non-trustworthy pass is retried, never accepted.
    """

    def test_pass_trustworthy_is_accept(self):
        res = cycle.classify("pass", trustworthy=True)
        self.assertEqual(res.decision, cycle.Decision.ACCEPT)

    def test_pass_not_trustworthy_is_retry(self):
        # The #3002 defense-in-depth pin: a pass over a truncated run is NEVER
        # accepted. The POISONED verdict is the primary representation; this is
        # the belt for the residual risk the writer let pass+untrusted through.
        res = cycle.classify("pass", trustworthy=False)
        self.assertEqual(res.decision, cycle.Decision.RETRY)

    def test_fail_is_bail(self):
        res = cycle.classify("fail")
        self.assertEqual(res.decision, cycle.Decision.BAIL)

    def test_refused_is_retry(self):
        res = cycle.classify("refused")
        self.assertEqual(res.decision, cycle.Decision.RETRY)

    def test_retry_is_retry(self):
        res = cycle.classify("retry")
        self.assertEqual(res.decision, cycle.Decision.RETRY)

    def test_poisoned_is_retry(self):
        # rc=0 + undercount banner -> POISONED -> retry (the #3001 distrust,
        # materialized as a verdict so the consumer checks one field).
        res = cycle.classify("poisoned")
        self.assertEqual(res.decision, cycle.Decision.RETRY)

    def test_unknown_verdict_returns_none(self):
        # An unknown/empty verdict -> None (the caller falls back to the legacy
        # rc-based decision). Returning None keeps "no verdict" distinguishable
        # from "verdict said retry" (the #3000 shape).
        self.assertIsNone(cycle.classify(""))
        self.assertIsNone(cycle.classify("nonsense"))
        self.assertIsNone(cycle.classify(None))

    def test_truth_table_all_five_verdicts_by_trustworthy(self):
        """The exhaustive 5x2 table — the canonical classify contract. The
        load-bearing cell is (pass, False) -> retry."""
        expected = {
            ("pass", True): cycle.Decision.ACCEPT,
            ("pass", False): cycle.Decision.RETRY,   # per #3002 — never accept
            ("fail", True): cycle.Decision.BAIL,
            ("fail", False): cycle.Decision.BAIL,
            ("refused", True): cycle.Decision.RETRY,
            ("refused", False): cycle.Decision.RETRY,
            ("retry", True): cycle.Decision.RETRY,
            ("retry", False): cycle.Decision.RETRY,
            ("poisoned", True): cycle.Decision.RETRY,
            ("poisoned", False): cycle.Decision.RETRY,
        }
        for v in verdict.Verdict:
            for tw in (True, False):
                with self.subTest(verdict=v.value, trustworthy=tw):
                    res = cycle.classify(v.value, trustworthy=tw)
                    self.assertIsNotNone(res, "{} should classify".format(v))
                    self.assertEqual(res.decision, expected[(v.value, tw)])

    def test_every_result_carries_a_reason(self):
        # The reason preserves the per-attempt message specificity the old inline
        # bash carried (which RETRY flavor — refused/coverage-short/poisoned —
        # determines the log line at the call site).
        for v in verdict.Verdict:
            res = cycle.classify(v.value, trustworthy=True)
            if res is not None:
                self.assertTrue(res.reason, "{} missing reason".format(v))


# ---------------------------------------------------------------------------
# 2. cross_check — conservative-non-accept over the 3x3 grid.
# ---------------------------------------------------------------------------

class CrossCheckTest(unittest.TestCase):
    """cross_check: reconcile two decisions, NEVER accepting on disagreement.

    Faithful port of gate_full_cycle.sh:150-157. The full 3x3 grid of
    (verdict_decision, legacy_decision) is pinned so any future change to the
    policy is immediately visible. The two load-bearing rows are the ones where
    one side is ACCEPT: a split involving accept MUST escalate to BAIL (the hard
    stop), never quietly retry — a false accept is the worst outcome.
    """

    def test_agree_returns_that_decision(self):
        for d in cycle.Decision:
            res = cycle.cross_check(
                cycle.Result(d, "v"), cycle.Result(d, "l"))
            self.assertEqual(res.decision, d)

    def test_disagreement_with_accept_escalates_to_bail(self):
        # The false-accept-prevention policy: if EITHER side is accept and they
        # disagree, BAIL (never accept, never quietly retry).
        for other in (cycle.Decision.RETRY, cycle.Decision.BAIL):
            with self.subTest(other=other):
                # verdict=accept, legacy=other
                res = cycle.cross_check(
                    cycle.Result(cycle.Decision.ACCEPT, "v"),
                    cycle.Result(other, "l"))
                self.assertEqual(res.decision, cycle.Decision.BAIL)
                # verdict=other, legacy=accept
                res = cycle.cross_check(
                    cycle.Result(other, "v"),
                    cycle.Result(cycle.Decision.ACCEPT, "l"))
                self.assertEqual(res.decision, cycle.Decision.BAIL)

    def test_disagreement_retry_vs_bail_verdict_wins(self):
        # When neither side is accept, the VERDICT decision wins the retry/bail
        # axis — the legacy decision is only a guard against a false accept, not
        # an authority on retry-vs-bail.
        res = cycle.cross_check(
            cycle.Result(cycle.Decision.RETRY, "v"),
            cycle.Result(cycle.Decision.BAIL, "l"))
        self.assertEqual(res.decision, cycle.Decision.RETRY)
        res = cycle.cross_check(
            cycle.Result(cycle.Decision.BAIL, "v"),
            cycle.Result(cycle.Decision.RETRY, "l"))
        self.assertEqual(res.decision, cycle.Decision.BAIL)

    def test_full_3x3_grid(self):
        """The exhaustive reconciliation table. Pinned in full so a policy
        change is immediately visible as a test diff.

        Grid (rows = verdict decision, cols = legacy decision):
                  accept  retry   bail
            accept  A       B       B
            retry   B       R       R
            bail    B       B       B
        """
        A, R, B = (cycle.Decision.ACCEPT, cycle.Decision.RETRY,
                   cycle.Decision.BAIL)
        expected = {
            (A, A): A, (A, R): B, (A, B): B,
            (R, A): B, (R, R): R, (R, B): R,
            (B, A): B, (B, R): B, (B, B): B,
        }
        for vd in cycle.Decision:
            for ld in cycle.Decision:
                with self.subTest(verdict=vd.value, legacy=ld.value):
                    res = cycle.cross_check(
                        cycle.Result(vd, "v"), cycle.Result(ld, "l"))
                    self.assertEqual(res.decision, expected[(vd, ld)],
                        "({}, {}) -> {}, expected {}".format(
                            vd.value, ld.value, res.decision.value,
                            expected[(vd, ld)].value))

    def test_disagreement_never_returns_accept(self):
        """The policy in one sentence: on disagreement, NEVER accept. Verified
        exhaustively — no disagreement cell in the grid yields ACCEPT."""
        for vd in cycle.Decision:
            for ld in cycle.Decision:
                if vd == ld:
                    continue  # agreement — accept is fine when both say accept
                res = cycle.cross_check(
                    cycle.Result(vd, "v"), cycle.Result(ld, "l"))
                self.assertNotEqual(res.decision, cycle.Decision.ACCEPT,
                    "disagreement ({},{}) returned accept — violates "
                    "conservative-non-accept".format(vd.value, ld.value))

    def test_none_verdict_returns_legacy(self):
        # An absent verdict-based decision (verdict file missing) -> the legacy
        # decision stands (the fallback path). cross_check returns it as a Result
        # (uniform .decision/.reason); the decision is what matters.
        legacy = cycle.Result(cycle.Decision.ACCEPT, "rc=0")
        res = cycle.cross_check(None, legacy)
        self.assertEqual(res.decision, cycle.Decision.ACCEPT)

    def test_none_legacy_defers_to_verdict(self):
        # Symmetric: an absent legacy (e.g. gate_wait's log with neither FAIL nor
        # PASS markers) is NOT a dissent — the verdict's decision stands. This is
        # the case that makes gate_wait fit cross_check: a present pass verdict
        # with no log markers defers to the verdict (accept), rather than vetoing.
        res = cycle.cross_check(cycle.Result(cycle.Decision.ACCEPT, "pass"), None)
        self.assertEqual(res.decision, cycle.Decision.ACCEPT)
        res = cycle.cross_check(cycle.Result(cycle.Decision.BAIL, "fail"), None)
        self.assertEqual(res.decision, cycle.Decision.BAIL)

    def test_both_none_bails(self):
        # Nothing classified (e.g. gate_wait: verdict missing AND no terminal log
        # markers) -> BAIL, fail closed. Treating "no decision" as accept is the
        # #3000 shape; the conservative default is the other direction.
        res = cycle.cross_check(None, None)
        self.assertEqual(res.decision, cycle.Decision.BAIL)

    def test_absent_legacy_does_not_mask_false_accept(self):
        # The policy still holds: when BOTH sides are present and disagree with an
        # accept, it escalates to BAIL. A None legacy only defers when it is
        # genuinely absent — it never OVERRIDES a present bail to produce accept.
        res = cycle.cross_check(cycle.Result(cycle.Decision.ACCEPT, "v"),
                                cycle.Result(cycle.Decision.BAIL, "l"))
        self.assertEqual(res.decision, cycle.Decision.BAIL)

    def test_accepts_bare_decision_words(self):
        # The CLI/bash passes decision WORDS, not Result objects. cross_check
        # coerces "accept"/"retry"/"bail" so the bash consumer does not have to
        # wrap. The reconciled word comes back via .decision.
        res = cycle.cross_check("accept", "retry")
        self.assertEqual(res.decision, cycle.Decision.BAIL)
        res = cycle.cross_check("retry", "retry")
        self.assertEqual(res.decision, cycle.Decision.RETRY)


# ---------------------------------------------------------------------------
# 3. assert_asan_ran — the ASan-evidence check.
# ---------------------------------------------------------------------------

class AssertAsanRanTest(unittest.TestCase):
    """assert_asan_ran: OBSERVED log evidence (authoritative) + INTENDED verdict
    oracle (cross-check). Both required; neither alone is sufficient.

    Faithful port of gate_full_cycle.sh:255-269. The log grep is authoritative
    (it catches a mode that silently degraded even when the verdict says asan);
    the verdict oracle is the cross-check (skipped when no verdict file exists,
    matching the bash `if [ -n "$_fvf" ]`)."""

    def _log(self, d, text):
        p = os.path.join(d, "full.log")
        with open(p, "w") as f:
            f.write(text)
        return p

    def test_log_has_asan_and_verdict_asan_passes(self):
        with tempfile.TemporaryDirectory() as d:
            lp = self._log(d, "... Running asan-instrumented shards ...\n")
            vobj = {"oracle": "asan"}
            ok, _ = cycle.assert_asan_ran(vobj, lp)
            self.assertTrue(ok)

    def test_log_has_sanitize_passes(self):
        # The grep is 'asan|sanitize' — either token satisfies the OBSERVED check.
        with tempfile.TemporaryDirectory() as d:
            lp = self._log(d, "AddressSanitizer: heap-buffer-overflow\n")
            ok, _ = cycle.assert_asan_ran({"oracle": "asan"}, lp)
            self.assertTrue(ok)

    def test_log_case_insensitive(self):
        # gate_full_cycle.sh:264 uses `grep -qi` — case-insensitive.
        with tempfile.TemporaryDirectory() as d:
            lp = self._log(d, "running ASAN shards\n")
            ok, _ = cycle.assert_asan_ran({"oracle": "asan"}, lp)
            self.assertTrue(ok)

    def test_log_no_sanitizer_fails_even_if_verdict_asan(self):
        # The belt: a mode that silently degraded to plain produces a verdict
        # saying oracle=asan (the INTENDED mode gate.sh set) but a log with NO
        # sanitizer output (the OBSERVED reality). This MUST fail — it is the
        # exact defect fix #3011's allow-list is the braces for. NOTE the log
        # text avoids the substrings 'asan' AND 'sanitize' (the bash grep -qi
        # 'asan\|sanitize' matches e.g. "sanitizer" — a faithful check, so the
        # fixture must not contain either token to represent a plain run).
        with tempfile.TemporaryDirectory() as d:
            lp = self._log(d, "plain shard run, nothing to see here\n")
            ok, reason = cycle.assert_asan_ran({"oracle": "asan"}, lp)
            self.assertFalse(ok)
            self.assertIn("no sanitizer", reason)

    def test_verdict_oracle_not_asan_fails(self):
        # The verdict's oracle field is the INTENDED mode cross-check. A verdict
        # that says oracle=plain for the --full pass means the ASan run's own
        # verdict does not confirm it ran as ASan.
        with tempfile.TemporaryDirectory() as d:
            lp = self._log(d, "asan evidence present\n")
            ok, reason = cycle.assert_asan_ran({"oracle": "plain"}, lp)
            self.assertFalse(ok)
            self.assertIn("expected asan", reason)

    def test_missing_verdict_skips_oracle_check(self):
        # gate_full_cycle.sh:256 `if [ -n "$_fvf" ]`: when no per-attempt verdict
        # file is found, the oracle cross-check is SKIPPED — only the log grep
        # decides. A missing verdict must not fail the check (back-compat with
        # the writer being best-effort).
        with tempfile.TemporaryDirectory() as d:
            lp = self._log(d, "asan evidence\n")
            ok, _ = cycle.assert_asan_ran(None, lp)
            self.assertTrue(ok)

    def test_missing_log_fails(self):
        # A missing/unreadable log has no evidence — fail CLOSED (the #3000
        # shape: 'could not read the log' is not 'log said asan').
        ok, _ = cycle.assert_asan_ran({"oracle": "asan"},
                                      "/nonexistent/full-xyz-12345.log")
        self.assertFalse(ok)

    def test_reason_on_success_and_failure(self):
        with tempfile.TemporaryDirectory() as d:
            lp = self._log(d, "asan\n")
            _, ok_reason = cycle.assert_asan_ran({"oracle": "asan"}, lp)
            self.assertIn("asan", ok_reason)
            lp2 = self._log(d, "nothing\n")
            _, fail_reason = cycle.assert_asan_ran({"oracle": "asan"}, lp2)
            self.assertTrue(fail_reason)


# ---------------------------------------------------------------------------
# 4. The 0-disagreement proof: classify == legacy on every reachable state.
# ---------------------------------------------------------------------------

class ZeroDisagreementProofTest(unittest.TestCase):
    """The mechanical 0-disagreement proof: for every REACHABLE gate state, the
    verdict-based decision (classify over verdict_from_rc) and the legacy
    $?/grep decision (legacy_decision) AGREE. Therefore cross_check never
    escalates on a reachable state — its BAIL escalation fires only on a
    UNREACHABLE disagreement (a bug in the verdict write or a race).

    This is the proof that the consolidated module preserves the policy the
    inline bash enforced: the cycle's reachable exit codes are 0, 1, 2, 3
    (gate.sh's taxonomy: pass/fail/preflight-refused/coverage-short); rc==64
    (bad mode) is unreachable from the cycle (it passes only valid modes).
    """

    def test_classify_equals_legacy_on_all_reachable_states(self):
        """For every (rc, undercount) the cycle can produce, the two decisions
        agree. The cycle's reachable exit codes are 0, 1, 2, 3."""
        disagreements = []
        for rc in (0, 1, 2, 3):
            for undercount in (False, True):
                truncated = undercount or (rc == 3)
                v = verdict.verdict_from_rc(rc, truncated)
                tw = verdict.is_trustworthy(v, truncated)
                new = cycle.classify(v.value, trustworthy=tw)
                legacy = cycle.legacy_decision(rc, undercount=undercount)
                if new.decision != legacy.decision:
                    disagreements.append(
                        "rc={}, undercount={}: classify({})={} but "
                        "legacy={}".format(rc, undercount, v.value,
                                           new.decision.value,
                                           legacy.decision.value))
        self.assertEqual(disagreements, [],
            "classify/legacy disagreement on reachable states:\n  " +
            "\n  ".join(disagreements))

    def test_cross_check_never_escalates_on_reachable_states(self):
        """Because classify == legacy on reachable states, cross_check's
        escalation (BAIL on accept-disagreement) never fires there — it returns
        the agreed decision. Verified by composing the two over the grid."""
        for rc in (0, 1, 2, 3):
            for undercount in (False, True):
                truncated = undercount or (rc == 3)
                v = verdict.verdict_from_rc(rc, truncated)
                tw = verdict.is_trustworthy(v, truncated)
                new = cycle.classify(v.value, trustworthy=tw)
                legacy = cycle.legacy_decision(rc, undercount=undercount)
                res = cycle.cross_check(new, legacy)
                self.assertEqual(res.decision, new.decision,
                    "rc={}: cross_check escalated on a reachable state".format(rc))

    def test_rc64_is_the_only_documented_divergence(self):
        """rc==64 (bad mode, #3011) is the ONLY rc where classify and legacy
        diverge: legacy bails (rc!=0, not 2/3), classify says refused->retry.
        Unreachable from the cycle (valid modes only) and harmless if it ever
        arises (retry -> exhaustion -> return 99, still no push). cross_check
        resolves it to RETRY (verdict wins the retry/bail axis; neither accept).
        Pinned so a future change notices the divergence."""
        v = verdict.verdict_from_rc(64)
        new = cycle.classify(v.value)
        legacy = cycle.legacy_decision(64)
        self.assertEqual(new.decision, cycle.Decision.RETRY)
        self.assertEqual(legacy.decision, cycle.Decision.BAIL)
        res = cycle.cross_check(new, legacy)
        self.assertEqual(res.decision, cycle.Decision.RETRY)

    def test_legacy_decision_matches_bash_function(self):
        """Faithful port of _legacy_rc_decision (gate_full_cycle.sh:77-83):
          rc==0 + undercount -> retry;  rc==0 clean -> accept;
          rc==2/3 -> retry;  else -> bail. Exhaustive over the bash's case."""
        cases = {
            (0, False): cycle.Decision.ACCEPT,
            (0, True): cycle.Decision.RETRY,
            (1, False): cycle.Decision.BAIL,
            (1, True): cycle.Decision.BAIL,
            (2, False): cycle.Decision.RETRY,
            (2, True): cycle.Decision.RETRY,
            (3, False): cycle.Decision.RETRY,
            (3, True): cycle.Decision.RETRY,
            (64, False): cycle.Decision.BAIL,
            (127, False): cycle.Decision.BAIL,
        }
        for (rc, uc), expected in cases.items():
            with self.subTest(rc=rc, undercount=uc):
                self.assertEqual(
                    cycle.legacy_decision(rc, undercount=uc).decision, expected)


# ---------------------------------------------------------------------------
# 5. CLI smoke tests — the runner scripts' entry points.
# ---------------------------------------------------------------------------

def _run_cli(args):
    scripts_dir = os.path.join(os.path.dirname(__file__), "..", "..")
    env = dict(os.environ, PYTHONPATH=scripts_dir)
    r = subprocess.run(
        [sys.executable, "-m", "gate.cycle"] + args,
        capture_output=True, text=True, env=env)
    return r


class CliClassifyTest(unittest.TestCase):
    """`classify --verdict V [--not-trustworthy]` -> decision word on stdout.

    The decision word must be byte-identical to the legacy bash
    `echo accept|retry|bail` — that is the dual-consume 0-disagreement
    assertion the runner scripts check (a string equality on these words)."""

    def test_classify_pass_is_accept(self):
        r = _run_cli(["classify", "--verdict", "pass"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "accept")

    def test_classify_fail_is_bail(self):
        r = _run_cli(["classify", "--verdict", "fail"])
        self.assertEqual(r.stdout.strip(), "bail")

    def test_classify_refused_is_retry(self):
        r = _run_cli(["classify", "--verdict", "refused"])
        self.assertEqual(r.stdout.strip(), "retry")

    def test_classify_retry_is_retry(self):
        r = _run_cli(["classify", "--verdict", "retry"])
        self.assertEqual(r.stdout.strip(), "retry")

    def test_classify_poisoned_is_retry(self):
        r = _run_cli(["classify", "--verdict", "poisoned"])
        self.assertEqual(r.stdout.strip(), "retry")

    def test_classify_pass_not_trustworthy_is_retry(self):
        # The #3002 defense-in-depth cell, via the CLI flag.
        r = _run_cli(["classify", "--verdict", "pass", "--not-trustworthy"])
        self.assertEqual(r.stdout.strip(), "retry")

    def test_classify_empty_verdict_prints_empty(self):
        # An absent verdict -> empty stdout (the caller falls back to legacy).
        # Command substitution strips the trailing newline -> "" in bash.
        r = _run_cli(["classify", "--verdict", ""])
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout.strip(), "")

    def test_classify_reason_to_stderr(self):
        # The reason goes to stderr so it does not pollute the decision token.
        r = _run_cli(["classify", "--verdict", "poisoned"])
        self.assertEqual(r.stdout.strip(), "retry")
        self.assertIn("poisoned", r.stderr)


class CliLegacyDecisionTest(unittest.TestCase):
    """`legacy-decision --rc R [--undercount]` -> decision word (the
    _legacy_rc_decision port)."""

    def test_rc0_clean_accept(self):
        r = _run_cli(["legacy-decision", "--rc", "0"])
        self.assertEqual(r.stdout.strip(), "accept")

    def test_rc0_undercount_retry(self):
        r = _run_cli(["legacy-decision", "--rc", "0", "--undercount"])
        self.assertEqual(r.stdout.strip(), "retry")

    def test_rc1_bail(self):
        r = _run_cli(["legacy-decision", "--rc", "1"])
        self.assertEqual(r.stdout.strip(), "bail")

    def test_rc2_retry(self):
        r = _run_cli(["legacy-decision", "--rc", "2"])
        self.assertEqual(r.stdout.strip(), "retry")

    def test_rc3_retry(self):
        r = _run_cli(["legacy-decision", "--rc", "3"])
        self.assertEqual(r.stdout.strip(), "retry")


class CliCrossCheckTest(unittest.TestCase):
    """`cross-check --verdict-decision D --legacy-decision D` -> reconciled word."""

    def test_agree_accept(self):
        r = _run_cli(["cross-check", "--verdict-decision", "accept",
                      "--legacy-decision", "accept"])
        self.assertEqual(r.stdout.strip(), "accept")

    def test_disagreement_accept_retry_bails(self):
        r = _run_cli(["cross-check", "--verdict-decision", "accept",
                      "--legacy-decision", "retry"])
        self.assertEqual(r.stdout.strip(), "bail")

    def test_disagreement_retry_bail_verdict_wins(self):
        r = _run_cli(["cross-check", "--verdict-decision", "retry",
                      "--legacy-decision", "bail"])
        self.assertEqual(r.stdout.strip(), "retry")

    def test_empty_verdict_decision_returns_legacy(self):
        r = _run_cli(["cross-check", "--verdict-decision", "",
                      "--legacy-decision", "accept"])
        self.assertEqual(r.stdout.strip(), "accept")


class CliAssertAsanRanTest(unittest.TestCase):
    """`assert-asan-ran --log-path L [--verdict-path P]` -> exit 0 iff ASan ran."""

    def _verdict(self, d, oracle="asan", rc=0):
        p = os.path.join(d, "full.verdict.json")
        obj = verdict.build_verdict_obj(rc=rc, mode="--full", oracle=oracle)
        verdict.write_verdict(p, obj)
        return p

    @staticmethod
    def _wlog(d, text):
        lp = os.path.join(d, "full.log")
        with open(lp, "w") as f:
            f.write(text)
        return lp

    def test_log_asan_verdict_asan_exits_zero(self):
        with tempfile.TemporaryDirectory() as d:
            lp = self._wlog(d, "asan-instrumented\n")
            vp = self._verdict(d)
            r = _run_cli(["assert-asan-ran", "--log-path", lp,
                          "--verdict-path", vp])
            self.assertEqual(r.returncode, 0, r.stderr)

    def test_log_no_sanitizer_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            lp = self._wlog(d, "plain run\n")
            vp = self._verdict(d)
            r = _run_cli(["assert-asan-ran", "--log-path", lp,
                          "--verdict-path", vp])
            self.assertEqual(r.returncode, 1)
            self.assertIn("no sanitizer", r.stderr)

    def test_verdict_oracle_plain_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            lp = self._wlog(d, "asan evidence\n")
            vp = self._verdict(d, oracle="plain")
            r = _run_cli(["assert-asan-ran", "--log-path", lp,
                          "--verdict-path", vp])
            self.assertEqual(r.returncode, 1)
            self.assertIn("expected asan", r.stderr)

    def test_no_verdict_path_log_asan_exits_zero(self):
        # The oracle cross-check is skipped when --verdict-path is absent.
        with tempfile.TemporaryDirectory() as d:
            lp = self._wlog(d, "asan evidence\n")
            r = _run_cli(["assert-asan-ran", "--log-path", lp])
            self.assertEqual(r.returncode, 0, r.stderr)


class CliUsageTest(unittest.TestCase):
    def test_no_subcommand_exits_nonzero(self):
        r = _run_cli([])
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("usage", r.stderr)


if __name__ == "__main__":
    unittest.main()
