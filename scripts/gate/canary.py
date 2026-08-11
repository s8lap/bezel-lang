"""canary.py — the canary tier's DECISION logic (#3010 Wave 4 / #2100).

A faithful Python port of gate.sh:615-670's decision chain — the classify/
dispatch that turns the diff-selected subset's result into a gate action. The
canary is the fail-fast subset run BEFORE the authoritative sharded pass: a red
canary fails the gate in minutes instead of a broken tree grinding every shard
to the timeout ceiling. A green canary proves nothing beyond its subset; the
full sharded pass below remains the SOLE acceptance authority, unconditionally.

WHAT MOVES HERE (the DECISION). The classify/dispatch — the pure logic that
maps (selector status, selection counts, ASan evidence, arbitration verdict) to
one of three gate actions. Before this module this was an inline bash if-elif-
case chain; now it is one unit-tested pure function with the fail-open/fail-hard
boundary provable as a truth table, not prose.

WHAT STAYS BASH (the SPAWN). The canary's run_parallel invocation, the ASan
grep, base resolution, and the gate_arbitrate call are bash glue — they produce
the INPUTS decide() consumes (asan_hits, arb_verdict, selector_ok, nsel, nuniv).
Only the DECISION (the mapping from those inputs to an action) is Python. This
matches the arbitration split (#2100): the re-run is Python, the suspect
extraction inputs it consumes are harvested by bash.

THE FAIL-OPEN / FAIL-HARD BOUNDARY (the load-bearing invariant). The canary
"must never brick a gate on its own bugs" (gate.sh header). The boundary:

  FAIL-OPEN (skip to the full pass — never brick on canary infra noise):
    - selector_ok=False     the selector/base failed — no selection was produced
    - nsel == 0              empty selection (no mapped src/test diff vs base)
    - max-pct exceeded       the selection is >= max_pct% of the suite — the full
                             pass does the work anyway, so the canary is redundant
    - arb_verdict == 2       nonzero exit but NO crash/FAIL marker (infra shape:
                             all-phantom list, shard dead before any test,
                             undercount, launch hiccup). The authoritative full
                             pass follows unconditionally and catches anything real.

  FAIL-HARD (brick — deterministic evidence of a real defect):
    - asan_hits > 0          a generated-code sanitizer report fired — deterministic
                             evidence of a memory error, no arbitration (#2100)
    - arb_verdict == 1       a suspect failed alone all 5 tries — a real regression
                             caught before the full pass grinds through it

  RUN-AUTHORITATIVE (canary is clean — continue to the full pass):
    - arb_verdict == 0       all suspects flaky (passed alone within 5 tries)
    - arb_verdict is None    the canary passed (crc==0) — no arbitration needed;
                             OR arbitration was not reached (asan check already
                             failed, but that path returns FAIL_HARD above)

DECISION PRIORITY. The checks fire in the bash if-elif-chain order; decide()
mirrors it exactly:
  1. selector_ok=False        -> SKIP (fail-open)
  2. nsel == 0                -> SKIP (fail-open)
  3. nuniv>0 and pct >= max   -> SKIP (fail-open, the max-pct gate)
  4. asan_hits > 0            -> FAIL_HARD
  5. arb_verdict == 1         -> FAIL_HARD
  6. arb_verdict == 2         -> SKIP (fail-open, infra shape)
  7. else                     -> RUN_AUTHORITATIVE

The max-pct arithmetic is bash integer division: 100 * nsel / nuniv >= max_pct
(gate.sh:632, `$(( 100 * nsel / nuniv )) -ge "$maxpct"`). For non-negative
counts Python's // (floor division) == bash truncation division, so
100 * nsel // nuniv is the faithful port. A nuniv of 0 means the selector
could not determine the universe; the bash does NOT skip on it ([ "$nuniv"
-gt 0 ] guards the pct check), so the canary runs — decide() mirrors that.

Stdlib only (no pytest). See test_canary.py for the load-bearing pins:
  - decide: the full decision truth table (every selector_ok x max-pct x
    arb_verdict x asan_hits combination, with the priority-dominance cases).
  - The fail-open/fail-hard boundary pins (every SKIP reason, every FAIL_HARD
    reason, every RUN reason).
  - The max-pct arithmetic boundary (exactly-at, just-below, nuniv=0).
  - CLI smoke tests (the dual-consume entry point the bash asserts against).
"""

import argparse
import enum
import sys


class CanaryDecision(enum.Enum):
    """The 3-way action the gate takes on a canary result.

    Three states, mirroring the three terminal paths in gate.sh:615-670's
    decision chain. Two of them continue to the authoritative full pass (the
    canary never IS the acceptance authority); one bricks the gate.
    """
    # The canary is clean (crc==0) or all suspects were flaky (arb 0) — continue
    # to the authoritative sharded pass. Maps to the bash fall-through / case 0.
    RUN_AUTHORITATIVE = "run"
    # Fail-OPEN: the canary hit a selector/infra/max-pct/infra-shape condition.
    # Warn loudly but continue — the full pass follows unconditionally and the
    # canary must never brick a gate on its own bugs. Maps to canary_skip() +
    # the case-2 infra-shape branch.
    SKIP_AND_RUN_AUTHORITATIVE = "skip"
    # Fail-HARD: deterministic evidence of a real defect (ASan fire or a real-
    # regression arbitration verdict). Bricks the gate before the full pass
    # grinds every shard to the timeout ceiling. Maps to fail "canary ASan" /
    # case-1.
    FAIL_HARD = "hard"


def decide(nsel, nuniv, max_pct, arb_verdict, asan_hits, selector_ok):
    """Classify a canary result -> CanaryDecision.

    Faithful port of gate.sh:615-670's decision chain (the classify/dispatch,
    NOT the spawn — the run_parallel invocation stays bash). The inputs:

      nsel         — selected test count (grep -c of the canary list; 0 = empty).
      nuniv        — universe test count (the selector's 'universe: N' line; 0
                     means the selector could not determine it).
      max_pct      — GATE_CANARY_MAX_PCT (default 50). Skip when the selection
                     exceeds this percentage of the suite.
      arb_verdict  — the gate_arbitrate result: 0 (all flaky), 1 (real
                     regression), 2 (infra shape / no crash-FAIL marker), or None
                     when arbitration did not run (the canary passed crc==0, or
                     the ASan check already failed before arbitration).
      asan_hits    — count of generated-code ASan/LSan report markers in the
                     canary log (0 = none / not in ASan mode).
      selector_ok  — True iff select_tests.py produced a selection (the base
                     resolved and the selector exited 0). False for no-base and
                     selector-failure (both fail-open).

    Returns a CanaryDecision. The priority mirrors the bash if-elif-chain order
    exactly (see the module docstring's DECISION PRIORITY list). The fail-open/
    fail-hard boundary is the load-bearing invariant: a wrong fail-HARD bricks
    the gate on canary infra noise (the forbidden outcome); a wrong fail-OPEN
    misses a real regression (the full pass catches it, but fail-fast is lost).
    Both directions are pinned by test_canary.py's truth table.

    per #2100 (the canary tier's design + the arbitration contract it shares
    with the full pass).
    """
    # per #2100: selector/base failure is FAIL-OPEN (the canary must never brick
    # on its own bugs). Covers both no-base and selector-failure — neither
    # produced a selection.
    if not selector_ok:
        return CanaryDecision.SKIP_AND_RUN_AUTHORITATIVE
    # Empty selection: no mapped src/test diff vs base — nothing to fail-fast on.
    # per gate.sh:630-631.
    if nsel == 0:
        return CanaryDecision.SKIP_AND_RUN_AUTHORITATIVE
    # Max-pct gate: the selection is large enough that the full pass does the
    # work anyway, so the canary is redundant. per gate.sh:632. Bash integer
    # division: for non-negative counts // == bash truncation. nuniv==0 does NOT
    # trigger (the selector couldn't size the universe — run the canary).
    if nuniv > 0 and (100 * nsel // nuniv) >= max_pct:
        return CanaryDecision.SKIP_AND_RUN_AUTHORITATIVE
    # per #2100: a fired generated-code sanitizer report is DETERMINISTIC
    # evidence of a memory error — fail hard, no arbitration. (This check
    # precedes arbitration in the bash too: gate.sh:642-648 before :650.)
    if asan_hits > 0:
        return CanaryDecision.FAIL_HARD
    # Arbitration dispatch (gate.sh:653-667). Only meaningful when the canary
    # had a nonzero exit (crc!=0); a green canary (crc==0) has arb_verdict=None
    # and falls through to RUN_AUTHORITATIVE below.
    if arb_verdict == 1:
        # A suspect failed alone all 5 tries — a real regression caught before
        # the full pass. per #1861 / #2100.
        return CanaryDecision.FAIL_HARD
    if arb_verdict == 2:
        # Infra shape: nonzero exit but NO crash/FAIL marker (all-phantom list,
        # shard dead before any test, undercount, launch hiccup). FAIL-OPEN per
        # the canary contract — the authoritative full pass follows
        # unconditionally and catches anything real. per gate.sh:656-665.
        return CanaryDecision.SKIP_AND_RUN_AUTHORITATIVE
    # arb_verdict == 0 (all suspects flaky — passed alone within 5 tries) or
    # None (canary passed crc==0 — no arbitration needed). Either way the
    # canary is clean: continue to the authoritative full pass.
    return CanaryDecision.RUN_AUTHORITATIVE


# ---------------------------------------------------------------------------
# CLI — the runner scripts call this as `python3 -m gate.canary decide ...`.
# stdout is the decision word (run/skip/hard) the bash consumer reads via $().
# The dual-consume 0-disagreement assertion is a string equality on these words.
# ---------------------------------------------------------------------------

def _main_decide(argv):
    """`decide --nsel N --nuniv N --max-pct P [--arb-verdict V] [--asan-hits H]
    [--no-selector-ok]` -> decision word on stdout.

    The decision the bash canary block cross-checks against its own dispatch.
    --arb-verdict defaults to -1 (not arbitrated); 0/1/2 are the gate_arbitrate
    contract. --asan-hits defaults to 0. --no-selector-ok sets selector_ok=False
    (default True). Prints run/skip/hard to stdout."""
    p = argparse.ArgumentParser(
        prog="canary.py decide",
        description="Classify a canary result -> run/skip/hard (#2100, #3010 "
                    "Wave 4). The fail-open/fail-hard decision the bash canary "
                    "block cross-checks against its own dispatch.")
    p.add_argument("--nsel", type=int, required=True,
                   help="selected test count (grep -c of the canary list)")
    p.add_argument("--nuniv", type=int, required=True,
                   help="universe test count (the selector's universe: N)")
    p.add_argument("--max-pct", type=int, required=True,
                   help="GATE_CANARY_MAX_PCT (skip when selection exceeds this pct)")
    p.add_argument("--arb-verdict", type=int, default=-1,
                   help="gate_arbitrate result: 0=flaky, 1=real, 2=infra-shape, "
                        "-1=not arbitrated (default)")
    p.add_argument("--asan-hits", type=int, default=0,
                   help="count of ASan/LSan report markers in the canary log")
    # selector_ok defaults True; --no-selector-ok flips it (the common case is a
    # working selector; the flag names the exceptional path for readability).
    p.add_argument("--no-selector-ok", dest="selector_ok", action="store_false",
                   default=True,
                   help="the selector/base failed (fail-open; default: ok)")
    args = p.parse_args(argv)
    # -1 is the CLI sentinel for "not arbitrated" (None in the Python API). An
    # explicit 0/1/2 is the gate_arbitrate contract.
    arb = args.arb_verdict if args.arb_verdict >= 0 else None
    decision = decide(args.nsel, args.nuniv, args.max_pct, arb,
                      args.asan_hits, args.selector_ok)
    print(decision.value)
    return 0


def main(argv):
    if len(argv) >= 2:
        cmd, rest = argv[1], argv[2:]
        if cmd == "decide":
            return _main_decide(rest)
    sys.stderr.write("usage: canary.py {decide} ...\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
