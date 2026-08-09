"""Tests for gate.canary (#3010 Wave 4 / #2100 — the canary decision logic).

The load-bearing pin groups:
  1. decide — the FULL decision truth table. Every reachable path through the
     priority chain, with the fail-open/fail-hard boundary pinned explicitly.
  2. Boundary pins — every SKIP reason, every FAIL_HARD reason, every RUN reason
     (the canary must never brick on its own bugs; both boundary directions
     preserved exactly).
  3. Max-pct arithmetic — the integer-division boundary (exactly-at, just-below,
     nuniv=0) that decides whether the canary is redundant with the full pass.
  4. Priority dominance — earlier checks override later ones (selector_ok=False
     dominates everything; asan_hits > 0 dominates arbitration).
  5. CLI smoke — the dual-consume entry point the bash asserts against.
"""

import os
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from gate import canary  # noqa: E402

RUN = canary.CanaryDecision.RUN_AUTHORITATIVE
SKIP = canary.CanaryDecision.SKIP_AND_RUN_AUTHORITATIVE
HARD = canary.CanaryDecision.FAIL_HARD


# ---------------------------------------------------------------------------
# 1. decide — every reachable terminal path through the priority chain.
# ---------------------------------------------------------------------------

class DecidePathTest(unittest.TestCase):
    """decide: the 8 reachable terminal paths, one test each.

    Each path corresponds to a distinct terminal exit in gate.sh:615-670's
    decision chain. The path name matches the bash comment / canary_skip reason.
    """

    # Path 1: selector_ok=False — covers BOTH no-base and selector-failure
    # (gate.sh:621-622 + :623-625). Both fail-open; selector_ok collapses them.
    def test_selector_failed_is_skip(self):
        self.assertEqual(
            canary.decide(0, 0, 50, None, 0, selector_ok=False), SKIP)
        # Even with a non-empty selection and ASan evidence, selector_ok=False
        # dominates — no selection was produced, so there is nothing to arbitrate.
        self.assertEqual(
            canary.decide(10, 100, 50, 1, 5, selector_ok=False), SKIP)

    def test_no_base_is_skip(self):
        # no-base is selector_ok=False (the selector never ran). Same path.
        self.assertEqual(
            canary.decide(0, 0, 50, None, 0, selector_ok=False), SKIP)

    # Path 2: empty selection (gate.sh:630-631).
    def test_empty_selection_is_skip(self):
        self.assertEqual(canary.decide(0, 1000, 50, None, 0, True), SKIP)

    # Path 3: max-pct exceeded (gate.sh:632-633).
    def test_max_pct_exceeded_is_skip(self):
        self.assertEqual(canary.decide(60, 100, 50, None, 0, True), SKIP)

    # Path 4: ASan fire — deterministic memory error (gate.sh:642-648).
    def test_asan_fire_is_hard(self):
        self.assertEqual(canary.decide(5, 100, 50, None, 1, True), HARD)
        self.assertEqual(canary.decide(5, 100, 50, None, 99, True), HARD)

    # Path 5: real regression — arb_verdict==1 (gate.sh:666-667).
    def test_arb_real_regression_is_hard(self):
        self.assertEqual(canary.decide(5, 100, 50, 1, 0, True), HARD)

    # Path 6: infra shape — arb_verdict==2, fail-open (gate.sh:656-665).
    def test_arb_infra_shape_is_skip(self):
        self.assertEqual(canary.decide(5, 100, 50, 2, 0, True), SKIP)

    # Path 7: all flaky — arb_verdict==0, continue (gate.sh:654-655).
    def test_arb_all_flaky_is_run(self):
        self.assertEqual(canary.decide(5, 100, 50, 0, 0, True), RUN)

    # Path 8: green canary — arb_verdict is None (crc==0, no arbitration).
    def test_green_canary_is_run(self):
        self.assertEqual(canary.decide(5, 100, 50, None, 0, True), RUN)


# ---------------------------------------------------------------------------
# 2. Boundary pins — the fail-open/fail-hard boundary, pinned explicitly.
# ---------------------------------------------------------------------------

class BoundaryPinTest(unittest.TestCase):
    """The fail-open/fail-hard boundary — the load-bearing invariant.

    per gate.sh header: the canary must never brick a gate on its own bugs.
    FAIL-OPEN (skip) cases must never produce FAIL_HARD; FAIL-HARD cases must
    never produce SKIP or RUN. Both directions preserved exactly.
    """

    # --- FAIL-OPEN pins (every reason the canary skips to the full pass) ---
    def test_fail_open_selector_error(self):
        d = canary.decide(0, 0, 50, None, 0, selector_ok=False)
        self.assertEqual(d, SKIP)

    def test_fail_open_empty_selection(self):
        d = canary.decide(0, 1000, 50, None, 0, True)
        self.assertEqual(d, SKIP)

    def test_fail_open_max_pct(self):
        d = canary.decide(51, 100, 50, None, 0, True)
        self.assertEqual(d, SKIP)

    def test_fail_open_infra_shape(self):
        # arb_verdict==2: nonzero exit, no crash/FAIL marker — the canary's own
        # infrastructure is misshaped, not a real regression.
        d = canary.decide(5, 100, 50, 2, 0, True)
        self.assertEqual(d, SKIP)

    # --- FAIL-HARD pins (every reason the canary bricks the gate) ---
    def test_fail_hard_asan_fire(self):
        d = canary.decide(5, 100, 50, None, 1, True)
        self.assertEqual(d, HARD)

    def test_fail_hard_real_regression(self):
        d = canary.decide(5, 100, 50, 1, 0, True)
        self.assertEqual(d, HARD)

    # --- RUN pins (the canary is clean — continue to the full pass) ---
    def test_run_green_canary(self):
        d = canary.decide(5, 100, 50, None, 0, True)
        self.assertEqual(d, RUN)

    def test_run_all_flaky(self):
        d = canary.decide(5, 100, 50, 0, 0, True)
        self.assertEqual(d, RUN)

    # --- The forbidden outcomes: FAIL-OPEN cases must NEVER be HARD ---
    def test_skip_cases_never_hard(self):
        """Every fail-open condition must produce SKIP, never HARD — a wrong
        fail-hard bricks the gate on canary infra noise (the forbidden outcome)."""
        skip_cases = [
            # (nsel, nuniv, max_pct, arb, asan, selector_ok)
            (0, 0, 50, None, 0, False),      # selector failed
            (0, 1000, 50, None, 0, True),     # empty selection
            (60, 100, 50, None, 0, True),     # max-pct
            (5, 100, 50, 2, 0, True),         # infra shape
        ]
        for args in skip_cases:
            with self.subTest(args=args):
                d = canary.decide(*args)
                self.assertEqual(d, SKIP,
                    "fail-open case {} produced {} (expected SKIP)".format(
                        args, d))

    # --- The forbidden outcomes: FAIL-HARD cases must NEVER be SKIP/RUN ---
    def test_hard_cases_never_skip_or_run(self):
        """Every fail-hard condition must produce HARD, never SKIP/RUN — a wrong
        fail-open misses a real regression (the full pass catches it, but the
        fail-fast purpose of the canary is lost)."""
        hard_cases = [
            (5, 100, 50, None, 1, True),      # ASan fire
            (5, 100, 50, 1, 0, True),          # real regression
        ]
        for args in hard_cases:
            with self.subTest(args=args):
                d = canary.decide(*args)
                self.assertEqual(d, HARD,
                    "fail-hard case {} produced {} (expected HARD)".format(
                        args, d))


# ---------------------------------------------------------------------------
# 3. Max-pct arithmetic — the integer-division boundary.
# ---------------------------------------------------------------------------

class MaxPctArithmeticTest(unittest.TestCase):
    """The max-pct gate: 100 * nsel // nuniv >= max_pct (gate.sh:632).

    Bash uses `$(( 100 * nsel / nuniv )) -ge "$maxpct"` — integer division. For
    non-negative counts Python's // (floor) == bash truncation. The boundary
    cases: exactly at the threshold (>= fires), just below, and nuniv=0 (the
    guard [ "$nuniv" -gt 0 ] means the check does NOT fire).
    """

    def test_exactly_at_threshold_skips(self):
        # 100 * 50 / 100 = 50 >= 50 -> skip. The >= (not >) is load-bearing.
        self.assertEqual(canary.decide(50, 100, 50, None, 0, True), SKIP)

    def test_just_below_threshold_runs(self):
        # 100 * 49 / 100 = 49 < 50 -> does not skip -> proceeds to RUN.
        self.assertEqual(canary.decide(49, 100, 50, None, 0, True), RUN)

    def test_integer_truncation_boundary(self):
        # 100 * 1 / 3 = 33 (truncated) < 50 -> runs. A float 33.33 < 50 too,
        # but the point is the truncation: 100*2/3 = 66 >= 50 -> skips.
        self.assertEqual(canary.decide(1, 3, 50, None, 0, True), RUN)
        self.assertEqual(canary.decide(2, 3, 50, None, 0, True), SKIP)

    def test_nuniv_zero_does_not_skip(self):
        # nuniv==0: the selector could not size the universe. The bash guard
        # [ "$nuniv" -gt 0 ] means the pct check does NOT fire — run the canary.
        # (nunniv=0 with nsel>0 is a real state: the selector found tests but
        # could not count the universe.)
        self.assertEqual(canary.decide(5, 0, 50, None, 0, True), RUN)

    def test_nuniv_zero_with_zero_nsel_is_empty_skip(self):
        # nsel==0 fires the empty-selection check BEFORE the max-pct check.
        self.assertEqual(canary.decide(0, 0, 50, None, 0, True), SKIP)

    def test_large_selection_overrides_asan_and_arb(self):
        # Max-pct skip fires BEFORE the spawn, so asan/arbr are irrelevant.
        # These inputs can't occur in the real gate (no spawn = no asan/arb),
        # but decide() must still return SKIP — the priority is load-bearing.
        self.assertEqual(canary.decide(60, 100, 50, 1, 5, True), SKIP)


# ---------------------------------------------------------------------------
# 4. Priority dominance — earlier checks override later ones.
# ---------------------------------------------------------------------------

class PriorityDominanceTest(unittest.TestCase):
    """decide's checks fire in the bash if-elif-chain order; later inputs are
    irrelevant when an earlier check already settled the decision.

    This is the structural guarantee that the migration preserved the chain
    ORDER, not just the individual checks. A reordering (e.g., checking asan
    before selector_ok) would pass the path tests but fail these dominance
    pins.
    """

    def test_selector_ok_dominates_everything(self):
        # selector_ok=False -> SKIP regardless of nsel, asan, arb. No selection
        # was produced; there is nothing to run or arbitrate.
        for args in [
            (10, 100, 50, None, 5, False),    # would be HARD if selector ok
            (10, 100, 50, 1, 0, False),        # would be HARD (real regression)
            (60, 100, 50, 1, 5, False),        # max-pct + hard + asan
        ]:
            with self.subTest(args=args):
                self.assertEqual(canary.decide(*args), SKIP)

    def test_empty_selection_dominates_asan_and_arb(self):
        # nsel==0 -> SKIP even if asan/arb say hard. No tests were selected.
        self.assertEqual(canary.decide(0, 100, 50, 1, 5, True), SKIP)

    def test_max_pct_dominates_asan_and_arb(self):
        # Max-pct skip fires before the spawn; asan/arb never produced.
        self.assertEqual(canary.decide(60, 100, 50, 1, 5, True), SKIP)

    def test_asan_dominates_arbitration(self):
        # ASan fire (asan_hits>0) is checked BEFORE arbitration in the bash
        # (gate.sh:642-648 before :650). A real-regression arb_verdict==1 with
        # asan_hits>0 still goes HARD (same outcome), but the point is the
        # ASan path is taken, NOT the arb path — asan is deterministic evidence
        # that needs no arbitration.
        self.assertEqual(canary.decide(5, 100, 50, 1, 3, True), HARD)
        # And asan overrides a would-be-flaky arb_verdict==0:
        self.assertEqual(canary.decide(5, 100, 50, 0, 3, True), HARD)

    def test_arb_verdict_none_is_run_when_clean(self):
        # None means "not arbitrated" (green canary). With no asan and passing
        # the selection gate, it's RUN.
        self.assertEqual(canary.decide(5, 100, 50, None, 0, True), RUN)


# ---------------------------------------------------------------------------
# 5. The full truth table — exhaustive over the post-selection-gate space.
# ---------------------------------------------------------------------------

class FullTruthTableTest(unittest.TestCase):
    """The exhaustive decision table over selector_ok x max-pct-gate x asan x arb.

    This is the canonical contract: every combination of the four decision
    inputs, mapped to its terminal action. The selector_ok=False and max-pct-gate
    rows dominate (pre-spawn fail-open); the remaining rows exercise the post-
    spawn space (asan x arb) that the canary spawn produces inputs for.
    """

    def test_post_spawn_truth_table_asan_x_arb(self):
        """The post-spawn decision space: asan_hits x arb_verdict.

        Pre-spawn checks pass (selector_ok=True, nsel/nuniv below max-pct), so
        every (asan, arb) combination reaches the post-spawn logic. asan>0
        dominates arb (checked first); arb=None/0 -> RUN; arb==1 -> HARD;
        arb==2 -> SKIP."""
        # (asan_hits, arb_verdict) -> expected decision
        table = {
            # asan=0: arb decides
            (0, None): RUN,    # green canary
            (0, 0):    RUN,    # all flaky
            (0, 1):    HARD,   # real regression
            (0, 2):    SKIP,   # infra shape
            # asan>0: dominates arb regardless of verdict
            (1, None): HARD,
            (1, 0):    HARD,
            (1, 1):    HARD,
            (1, 2):    HARD,
            (5, None): HARD,
            (5, 2):    HARD,
        }
        for (asan, arb), expected in table.items():
            with self.subTest(asan=asan, arb=arb):
                d = canary.decide(5, 100, 50, arb, asan, True)
                self.assertEqual(d, expected)

    def test_pre_spawn_dominance_selector_ok_false(self):
        """selector_ok=False dominates ALL post-spawn inputs."""
        for asan in (0, 1, 5):
            for arb in (None, 0, 1, 2):
                with self.subTest(asan=asan, arb=arb):
                    d = canary.decide(10, 100, 50, arb, asan, selector_ok=False)
                    self.assertEqual(d, SKIP)

    def test_pre_spawn_dominance_empty_selection(self):
        """nsel==0 dominates ALL post-spawn inputs (selector_ok=True)."""
        for asan in (0, 1, 5):
            for arb in (None, 0, 1, 2):
                with self.subTest(asan=asan, arb=arb):
                    d = canary.decide(0, 1000, 50, arb, asan, True)
                    self.assertEqual(d, SKIP)

    def test_pre_spawn_dominance_max_pct(self):
        """max-pct exceeded dominates ALL post-spawn inputs."""
        for asan in (0, 1, 5):
            for arb in (None, 0, 1, 2):
                with self.subTest(asan=asan, arb=arb):
                    d = canary.decide(60, 100, 50, arb, asan, True)
                    self.assertEqual(d, SKIP)


# ---------------------------------------------------------------------------
# 6. CLI smoke tests — the dual-consume entry point.
# ---------------------------------------------------------------------------

class CliSmokeTest(unittest.TestCase):
    """The `python3 -m gate.canary decide` CLI — the bash dual-consume entry.

    The bash canary block calls this and asserts its decision word matches the
    old bash dispatch. The words are run/skip/hard (CanaryDecision values).
    """

    def _decide(self, *cli_args):
        """Run the decide CLI, return the stdout decision word."""
        result = subprocess.run(
            [sys.executable, "-m", "gate.canary", "decide"] + list(cli_args),
            capture_output=True, text=True,
            cwd=os.path.join(os.path.dirname(__file__), "..", ".."),
            env={**os.environ, "PYTHONPATH": os.path.join(
                os.path.dirname(__file__), "..", "..")})
        self.assertEqual(result.returncode, 0,
                         "CLI failed: stderr={}".format(result.stderr))
        return result.stdout.strip()

    def test_cli_run_green_canary(self):
        word = self._decide("--nsel", "5", "--nuniv", "100", "--max-pct", "50")
        self.assertEqual(word, "run")

    def test_cli_skip_selector_failed(self):
        word = self._decide("--nsel", "0", "--nuniv", "0", "--max-pct", "50",
                            "--no-selector-ok")
        self.assertEqual(word, "skip")

    def test_cli_skip_empty_selection(self):
        word = self._decide("--nsel", "0", "--nuniv", "1000", "--max-pct", "50")
        self.assertEqual(word, "skip")

    def test_cli_skip_max_pct(self):
        word = self._decide("--nsel", "60", "--nuniv", "100", "--max-pct", "50")
        self.assertEqual(word, "skip")

    def test_cli_hard_asan(self):
        word = self._decide("--nsel", "5", "--nuniv", "100", "--max-pct", "50",
                            "--asan-hits", "1")
        self.assertEqual(word, "hard")

    def test_cli_hard_real_regression(self):
        word = self._decide("--nsel", "5", "--nuniv", "100", "--max-pct", "50",
                            "--arb-verdict", "1")
        self.assertEqual(word, "hard")

    def test_cli_skip_infra_shape(self):
        word = self._decide("--nsel", "5", "--nuniv", "100", "--max-pct", "50",
                            "--arb-verdict", "2")
        self.assertEqual(word, "skip")

    def test_cli_run_all_flaky(self):
        word = self._decide("--nsel", "5", "--nuniv", "100", "--max-pct", "50",
                            "--arb-verdict", "0")
        self.assertEqual(word, "run")

    def test_cli_arb_verdict_negative_one_is_none(self):
        # --arb-verdict -1 (the default) is the "not arbitrated" sentinel.
        word = self._decide("--nsel", "5", "--nuniv", "100", "--max-pct", "50",
                            "--arb-verdict", "-1")
        self.assertEqual(word, "run")

    def test_cli_matches_api_on_all_paths(self):
        """The CLI decision word must match the Python API on every path."""
        cases = [
            # (nsel, nuniv, max_pct, arb, asan, selector_ok) -> expected word
            (0, 0, 50, None, 0, False, "skip"),
            (0, 1000, 50, None, 0, True, "skip"),
            (60, 100, 50, None, 0, True, "skip"),
            (5, 100, 50, None, 1, True, "hard"),
            (5, 100, 50, 1, 0, True, "hard"),
            (5, 100, 50, 2, 0, True, "skip"),
            (5, 100, 50, 0, 0, True, "run"),
            (5, 100, 50, None, 0, True, "run"),
        ]
        for nsel, nuniv, mp, arb, asan, sel_ok, expected in cases:
            with self.subTest(nsel=nsel, arb=arb, asan=asan, sel_ok=sel_ok):
                cli_args = ["--nsel", str(nsel), "--nuniv", str(nuniv),
                            "--max-pct", str(mp)]
                if arb is not None:
                    cli_args += ["--arb-verdict", str(arb)]
                if asan:
                    cli_args += ["--asan-hits", str(asan)]
                if not sel_ok:
                    cli_args += ["--no-selector-ok"]
                word = self._decide(*cli_args)
                self.assertEqual(word, expected)
                # And the API agrees:
                api_decision = canary.decide(
                    nsel, nuniv, mp, arb, asan, sel_ok)
                self.assertEqual(api_decision.value, expected)


if __name__ == "__main__":
    unittest.main()
