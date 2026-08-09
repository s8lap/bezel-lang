"""cycle.py — the shared gate-cycle classifier (#3010 Wave 3).

Consolidates the recurring "read a verdict/signal -> classify accept/retry/bail
-> conservative-on-disagreement" pattern that, before this module, was inlined
in FOUR places across the runner scripts:

  1. gate_full_cycle.sh run_until_trustworthy (the verdict->decision case at
     :136-143 + the _legacy_rc_decision cross-check at :77-83/:150-157).
  2. gate_and_push.sh verdict-before-push (the 3-way read-CLI reconcile at
     :108-122).
  3. gate_wait.sh classifier (verdict-pass + log-no-FAIL cross-check + the
     log-grep fallback ladder at :79-102).
  4. gate_full_cycle.sh ASan-ran evidence check (oracle-verdict + log grep at
     :255-269).

Four inline bash duplications of the same logic -> one unit-tested module, so a
future change to the policy lands in one place with one truth-table test.

THE PATTERN. Each site harvests a VERDICT (the authoritative gate-verdict.json
value, or the read-CLI exit code that encodes pass+trustworthy) plus some LOG
MARKERS (the undercount banner, FAIL markers), maps them to a 3-way decision
(accept/retry/bail), and reconciles two independent derivations CONSERVATIVELY:
on disagreement, NEVER accept. That conservative-non-accept policy is the
false-accept-prevention that stopped the 55-truncated-gates-over-24-days class
(a `== GATE PASS ==` banner printed over a run where 15 of 16 shards had died
and 5434 of 15123 tests actually ran — rc==0 was not sufficient). Consolidating
it here means the policy is provable, not prose.

This module composes on gate.verdict (#3002): it CONSUMES the verdict value/
file that gate.verdict writes; it does not re-derive rc->verdict itself (that is
verdict.verdict_from_rc's job, and the bash consumer already has the verdict
value in hand from `gate.verdict read --field verdict`).

Stdlib only (no pytest). See test_cycle.py for the load-bearing pins:
  - classify: the verdict->decision truth table over all 5 verdicts x trustworthy
    (the load-bearing cell is pass+NOT-trustworthy -> retry, never accept).
  - cross_check: conservative-non-accept over ALL 3x3 decision pairs.
  - assert_asan_ran: the oracle-verdict + log-grep evidence check.
  - the 0-disagreement proof: classify(verdict_from_rc(rc)) == legacy_decision(rc)
    on every reachable gate state (rc in {0,1,2,3} x truncation), so
    cross_check never escalates on a reachable state.
"""

import argparse
import enum
import os
import sys
from collections import namedtuple

# Reuse the verdict VALUE enum from verdict.py (#3002). classify CONSUMES the
# verdict that gate.verdict writes; it does not re-derive it. Importing the enum
# keeps the verdict-value strings in one source of truth (if verdict.py renames
# "poisoned" -> something else, this module's mapping moves with it).
from gate import verdict as verdict_mod
from gate import verdict as _v  # short alias for the case table


class Decision(enum.Enum):
    """The 3-way action a gate-cycle consumer takes on a verdict.

    Three states, not five: the five Verdict values (pass/fail/retry/refused/
    poisoned) collapse to three ACTIONS at the consumer (refused/retry/poisoned
    are all 'retry' — the cycle loops; only pass->accept and fail->bail are
    terminal). This is the decision space gate_full_cycle's run_until_trustworthy
    has always acted on (its `case "$dec"` at :162-193).
    """
    ACCEPT = "accept"  # trustworthy green — proceed (cycle returns 0; gate_and_push pushes).
    RETRY = "retry"    # transient (refused/retry/poisoned) — loop another attempt.
    BAIL = "bail"      # real failure — stop, return the rc, no push.


# A classification result: a Decision + the human-readable reason that decision
# was reached. The reason preserves the per-attempt message specificity the old
# inline bash carried (which retry flavor — refused / coverage-short / poisoned
# — determines the log line and the sleep duration at the call site).
Result = namedtuple("Result", ["decision", "reason"])


# NOTE: classify takes NO log-marker argument. The undercount banner's distrust
# is already materialized in the POISONED verdict (rc=0 + undercount -> poisoned
# -> retry), and the FAIL-marker override lives in gate_wait.sh's cross-check +
# cycle.assert_asan_ran. An earlier draft carried a LogMarkers namespace here
# that was computed but never read — dead code, removed. The load-bearing
# undercount signal for the LEGACY decision stays in legacy_decision's own
# `undercount` parameter (which IS used).


def classify(verdict_value, trustworthy=True):
    """Map a verdict VALUE (+ markers + trust) to a Result, or None.

    THE VERDICT-PATH DECISION. Faithful port of gate_full_cycle.sh's
    run_until_trustworthy verdict case (:136-143) PLUS the defense-in-depth
    trustworthy check pinned by test_integration.py's ConsumerDecisionMappingTest
    (pass+NOT-trustworthy -> retry, never accept — the residual-risk guard on
    top of the POISONED verdict, which is how rc0+truncated is normally
    represented; this catches the case where the writer somehow let a
    pass+untrustworthy document through).

    Mapping:
      pass + trustworthy     -> ACCEPT   (rc==0, clean — the only green path)
      pass + NOT trustworthy -> RETRY    (#3002 defense-in-depth; never accept
                                          an untrusted pass)
      refused                -> RETRY    (rc==2/64 — preflight refused)
      retry                  -> RETRY    (rc==3 — coverage short, #3002)
      poisoned               -> RETRY    (rc==0 + undercount, #3001)
      fail                   -> BAIL     (rc==1 — real regression)
      <unknown/empty>        -> None     (verdict absent/unparseable — the caller
                                          FALLS BACK to the legacy rc-based
                                          decision; this is gate_full_cycle's
                                          `dec=""` -> `dec="$old_dec"` at :159)

    `verdict_value` is the verdict field string gate.verdict emits ("pass"/
    "fail"/"retry"/"refused"/"poisoned"), or "" when the verdict file is
    missing/unparseable (the read CLI returned exit 2). `trustworthy` is the
    verdict file's trust flag (default True — by construction verdict=="pass"
    implies trustworthy, since verdict_from_rc only mints PASS for rc0+clean;
    the flag is the independent safety net).
    """
    if verdict_value == _v.Verdict.PASS.value:
        if trustworthy:
            return Result(Decision.ACCEPT, "pass")
        # #3002 defense-in-depth: a pass the writer let through over a truncated
        # run is NEVER accepted. The POISONED verdict is the primary
        # representation of this; this branch is the belt for the residual risk
        # that the verdict field says pass while integrity says otherwise.
        return Result(Decision.RETRY,
                      "pass verdict but trustworthy=false (undercount) — #3002")
    if verdict_value == _v.Verdict.FAIL.value:
        return Result(Decision.BAIL, "fail")
    if verdict_value == _v.Verdict.REFUSED.value:
        return Result(Decision.RETRY, "refused — preflight")
    if verdict_value == _v.Verdict.RETRY.value:
        return Result(Decision.RETRY, "retry — coverage short")
    if verdict_value == _v.Verdict.POISONED.value:
        return Result(Decision.RETRY,
                      "poisoned — rc=0 undercount banner (#3001)")
    # Unknown/empty verdict: no verdict-based decision. The caller falls back to
    # the legacy rc-based decision (gate_full_cycle.sh:159). Returning None
    # rather than a default keeps "no verdict" distinguishable from "verdict said
    # retry" — the #3000 shape (treating "no document" and "document said X"
    # alike is the defect).
    return None


def legacy_decision(rc, undercount=False):
    """The legacy $?/banner-grep decision (faithful port of _legacy_rc_decision).

    Ports gate_full_cycle.sh:77-83 exactly:
      rc==0 + undercount  -> RETRY   (distrust rc=0+undercount-banner, #3001)
      rc==0, clean        -> ACCEPT
      rc==2 or rc==3      -> RETRY   (preflight refused / coverage short)
      rc==1 or other      -> BAIL    (real failure / unknown — fail-closed)

    Kept as the CROSS-CHECK input to cross_check (two independent derivations
    from the same signals must agree) and the FALLBACK when the verdict file is
    missing (the writer is best-effort, per #3002). `undercount` is the
    'shard crash / undercount' banner grep result the legacy function took as
    its second argument (the log path is the caller's concern; this takes the
    resolved boolean).
    """
    if rc == 0:
        return (Result(Decision.RETRY, "rc=0 + undercount banner (#3001)")
                if undercount else Result(Decision.ACCEPT, "rc=0"))
    if rc == 2 or rc == 3:
        why = "preflight refused (rc=2)" if rc == 2 else "coverage short (rc=3)"
        return Result(Decision.RETRY, why)
    return Result(Decision.BAIL, "rc={} (real failure)".format(rc))


def cross_check(verdict_result, legacy_result):
    """Reconcile two decisions: conservative-non-accept-on-disagreement.

    Faithful port of gate_full_cycle.sh:150-157. The verdict-based decision and
    the legacy $?/grep decision are derived from the SAME signals inside
    gate.sh (rc + undercount banner), so on every REACHABLE state they agree
    (pinned by test_cycle.py's 0-disagreement proof). Disagreement means a bug
    in the verdict write or a race on the verdict file — and the policy is the
    one that stopped the 55-truncated-gates class: NEVER accept on disagreement
    (a false accept is the worst outcome — it pushed 55 gates that printed PASS
    over truncated runs).

    Reconciliation over the 3x3 decision grid (both sides present):
      agree                          -> that decision
      disagree + either is ACCEPT    -> BAIL  (escalate to the hard stop; the
                                               worst outcome is a false accept,
                                               so an accept-vs-anything split
                                               fails LOUDLY, not quietly retries)
      disagree + neither ACCEPT      -> the VERDICT decision (verdict wins the
                                        retry/bail axis; the legacy decision is
                                        only a guard against a false ACCEPT, not
                                        an authority on retry-vs-bail)
    Plus the absent-opinion cases (a side with no decision is NOT a dissent):
      one side None                  -> the OTHER side's decision (defer)
      both None                      -> BAIL  (nothing classified; fail closed)

    Accepts Result (or None for an absent/no-opinion decision) and Decision/word
    values (coerced via _coerce for ergonomics). Always returns a Result.

    ABSENT vs DISAGREE: a None input means "this side has no opinion" (a missing
    verdict file, or a log with neither FAIL nor PASS markers). A disagreement
    requires BOTH sides to have an opinion, so a lone None is NOT a disagreement
    — the other side's decision stands (defer). This is symmetric (verdict-absent
    defers to legacy; legacy-absent defers to verdict) and does NOT violate the
    conservative-non-accept policy: accepting a lone verdict's accept is not
    "accepting on a disagreement", because there is no dissenting opinion. The
    policy only escalates when two PRESENT opinions split. (Both absent -> BAIL:
    nothing was classified, fail closed.)
    """
    vd = _coerce(verdict_result)
    ld = _coerce(legacy_result)
    # A side with no opinion defers to the other. Both absent -> BAIL (nothing
    # classified; the #3000 shape — treating "no decision" as accept is the defect).
    if vd is None:
        return _as_result(legacy_result)
    if ld is None:
        return _as_result(verdict_result)
    if vd == ld:
        # Agree — the decision is settled. Return the verdict side (it carries
        # the more specific reason when the caller passed a Result).
        return _as_result(verdict_result)
    # Disagreement. Never accept (the false-accept-prevention policy).
    if vd == Decision.ACCEPT or ld == Decision.ACCEPT:
        return Result(
            Decision.BAIL,
            "cross-check disagreement (verdict={}, legacy={}) — escalated to "
            "bail per conservative-non-accept".format(vd.value, ld.value))
    # Disagreement on the retry/bail axis — verdict wins. The legacy decision is
    # a guard against a false ACCEPT, not an authority on retry-vs-bail, so when
    # neither side is accept the verdict (the authoritative source) stands.
    return _as_result(verdict_result)


def _coerce(value):
    """Normalize a Result / Decision / None / raw-string input to a Decision
    (or None). Lets cross_check accept either a Result (from classify) or a bare
    Decision/word (the bash CLI passes decision words), without forcing every
    caller to wrap."""
    if value is None:
        return None
    if isinstance(value, Decision):
        return value
    if isinstance(value, Result):
        return value.decision
    # Raw decision word ("accept"/"retry"/"bail") — from the CLI / a bash $().
    try:
        return Decision(value)
    except ValueError:
        return None


def _as_result(value):
    """Coerce a Result / Decision / word / None into a Result.

    cross_check always returns a Result so the CLI printer (and any caller) has
    a uniform `.decision` / `.reason`. A bare Decision/word gets a minimal
    reason (the word itself); None (no decision at all) degrades to BAIL — fail
    closed, the conservative default."""
    if isinstance(value, Result):
        return value
    d = _coerce(value)
    if d is None:
        return Result(Decision.BAIL, "no decision available")
    return Result(d, value if isinstance(value, str) else d.value)


# The FAIL-marker regexes gate_wait.sh cross-checks against (terminal markers
# only — NOT '## GATE FAILURE:' which is a shard-phase warning that can precede
# arbitration and is not itself a verdict). per gate_wait.sh:55,83.
_FAIL_MARKER_RE = "== GATE FAIL|GATE FAILED -> NOT pushing"
# The ASan-evidence grep (gate_full_cycle.sh:264). Case-insensitive. The log is
# AUTHORITATIVE here: it checks OBSERVED sanitizer output, which catches a mode
# that silently degraded to plain even when the verdict's oracle field says asan.
_ASAN_MARKER_RE = "asan|sanitize"


def _log_has(pattern, log_path):
    """Return True iff `log_path` contains a case-insensitive match for `pattern`.

    A missing/unreadable log has no markers (returns False) — matching the bash
    `2>/dev/null` swallow. The #3000 shape is the reason for the closed default:
    'could not read the log' must not be treated as 'log said pass'.
    """
    try:
        with open(log_path, errors="replace") as f:
            text = f.read()
    except OSError:
        return False
    import re
    return re.search(pattern, text, re.IGNORECASE) is not None


def assert_asan_ran(verdict_obj, log_path):
    """True iff evidence shows the ASan oracle actually ran.

    Faithful port of gate_full_cycle.sh:255-269. Two independent signals, BOTH
    required (neither alone is sufficient — per #2633 a mode that silently
    degraded is the exact defect this catches):

      1. OBSERVED (authoritative): the gate log mentions 'asan' or 'sanitize'.
         This is the belt to the verdict's braces: the verdict's oracle field is
         the INTENDED mode (gate.sh set ASAN=1 -> oracle=asan), while the log
         marker is the OBSERVED evidence. A mode that silently ran the weaker
         oracle under the stronger name passes check 2 (verdict says asan) but
         fails this one — fix #3011's allow-list is the braces; this is the belt.

      2. INTENDED (cross-check): when a verdict object is available, its `oracle`
         field must == "asan". A missing verdict (no per-attempt verdict file
         found) skips this check, exactly as the bash does at :256
         (`if [ -n "$_fvf" ]`).

    Returns (ok, reason). `ok` is True iff every available check passes. per
    #2917/#2633: plain and ASan are DIFFERENT oracles, neither dominating, so a
    cycle that silently ran one twice must NOT report dual-oracle green.
    """
    # Check 1 — OBSERVED evidence in the log (authoritative).
    if not _log_has(_ASAN_MARKER_RE, log_path):
        return (False,
                "the --full log mentions no sanitizer (asan|sanitize) — the "
                "ASan oracle did NOT run (silent degradation to plain)")
    # Check 2 — INTENDED mode in the verdict (cross-check; skipped if absent).
    if verdict_obj is not None:
        oracle = verdict_obj.get("oracle", "") if isinstance(
            verdict_obj, dict) else ""
        if oracle != "asan":
            return (False,
                    "the --full verdict says oracle='{}' (expected asan) — the "
                    "ASan oracle's verdict does not confirm it ran".format(
                        oracle or "n/a"))
    return (True, "asan evidence present (log marker + verdict oracle)")


# ---------------------------------------------------------------------------
# CLI — the runner scripts call these as `python3 -m gate.cycle <sub> ...`.
# Each subcommand is a thin wrapper over the pure functions above; stdout is the
# value the bash consumer reads via $(). Decisions print as the lowercase word
# (accept/retry/bail) so `dec=$(python3 -m gate.cycle classify ...)` matches the
# legacy `echo accept|retry|bail` byte-for-byte (the dual-consume 0-disagreement
# assertion is a string equality on these words). The reason prints to stderr so
# it does not pollute the decision token on stdout.
# ---------------------------------------------------------------------------

def _print_result(res, stream=sys.stdout):
    """Print a Result/None's decision word to stdout (empty line for None) and
    its reason to stderr. The empty-line-for-None contract is how the bash
    consumer detects 'verdict absent -> fall back to legacy' (command
    substitution strips the trailing newline -> "" in bash)."""
    if res is None:
        print("", file=stream)
        return
    print(res.decision.value, file=stream)
    if res.reason:
        sys.stderr.write("cycle: {}\n".format(res.reason))


def _main_classify(argv):
    """`classify --verdict V [--not-trustworthy]` -> decision word.

    The verdict-path decision. --verdict is the gate.verdict verdict field value
    (pass/fail/retry/refused/poisoned), or empty for an absent verdict. Prints
    accept/retry/bail to stdout (empty for an unknown/absent verdict -> the
    caller falls back to legacy-decision)."""
    p = argparse.ArgumentParser(
        prog="cycle.py classify",
        description="Classify a gate verdict value -> accept/retry/bail "
                    "(#3010 Wave 3). Prints the decision word to stdout; "
                    "empty for an absent/unknown verdict.")
    p.add_argument("--verdict", default="",
                   help="verdict field value (pass/fail/retry/refused/poisoned)")
    p.add_argument("--not-trustworthy", action="store_true", default=False,
                   help="the verdict's trustworthy flag is False (defense-in-"
                        "depth: pass+untrusted -> retry, never accept)")
    args = p.parse_args(argv)
    res = classify(args.verdict, trustworthy=not args.not_trustworthy)
    _print_result(res)
    return 0


def _main_legacy_decision(argv):
    """`legacy-decision --rc R [--undercount]` -> decision word.

    The legacy $?/grep decision (the _legacy_rc_decision port). Prints
    accept/retry/bail to stdout. Kept as the cross-check input and the
    verdict-absent fallback."""
    p = argparse.ArgumentParser(
        prog="cycle.py legacy-decision",
        description="The legacy $?/grep decision (faithful port of "
                    "_legacy_rc_decision). accept/retry/bail on stdout.")
    p.add_argument("--rc", type=int, required=True, help="gate.sh exit code")
    p.add_argument("--undercount", action="store_true", default=False,
                   help="the 'shard crash / undercount' banner was in the log")
    args = p.parse_args(argv)
    _print_result(legacy_decision(args.rc, undercount=args.undercount))
    return 0


def _main_cross_check(argv):
    """`cross-check --verdict-decision D --legacy-decision D` -> reconciled word.

    Conservative-non-accept-on-disagreement. Each D is accept/retry/bail (or
    empty for an absent verdict-based decision). Prints the reconciled decision
    to stdout. This is the load-bearing policy: on disagreement, never accept."""
    p = argparse.ArgumentParser(
        prog="cycle.py cross-check",
        description="Reconcile verdict + legacy decisions conservatively: on "
                    "disagreement, never accept (#3010 Wave 3).")
    p.add_argument("--verdict-decision", default="",
                   help="the verdict-based decision (accept/retry/bail, or empty)")
    p.add_argument("--legacy-decision", default="",
                   help="the legacy $?/grep decision (accept/retry/bail)")
    args = p.parse_args(argv)
    vd = None if args.verdict_decision == "" else args.verdict_decision
    res = cross_check(vd, args.legacy_decision)
    _print_result(res)
    return 0


def _main_assert_asan_ran(argv):
    """`assert-asan-ran --log-path L [--verdict-path P]` -> exit 0 iff ASan ran.

    The ASan-ran evidence check (gate_full_cycle.sh:255-269). --verdict-path is
    optional (the oracle cross-check is skipped when no verdict file is found,
    matching the bash). Exit 0 iff every available check passes; 1 (reason to
    stderr) otherwise. This is the evidence check that stops a cycle reporting
    dual-oracle green when it ran one oracle twice."""
    p = argparse.ArgumentParser(
        prog="cycle.py assert-asan-ran",
        description="Assert the ASan oracle actually ran (log evidence + "
                    "verdict oracle cross-check). Exit 0 iff it did.")
    p.add_argument("--log-path", required=True, help="the --full gate log path")
    p.add_argument("--verdict-path", default=None,
                   help="the --full verdict file (optional; the oracle "
                        "cross-check is skipped when absent)")
    args = p.parse_args(argv)

    vobj = None
    if args.verdict_path:
        vobj = verdict_mod.read_verdict(args.verdict_path)
    ok, reason = assert_asan_ran(vobj, args.log_path)
    if ok:
        return 0
    sys.stderr.write("cycle: assert-asan-ran FAILED — {}\n".format(reason))
    return 1


def main(argv):
    if len(argv) >= 2:
        cmd, rest = argv[1], argv[2:]
        if cmd == "classify":
            return _main_classify(rest)
        if cmd == "legacy-decision":
            return _main_legacy_decision(rest)
        if cmd == "cross-check":
            return _main_cross_check(rest)
        if cmd == "assert-asan-ran":
            return _main_assert_asan_ran(rest)
    sys.stderr.write(
        "usage: cycle.py {classify|legacy-decision|cross-check|"
        "assert-asan-ran} ...\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
