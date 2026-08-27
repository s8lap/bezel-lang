"""verdict.py — the authoritative gate verdict file (Phase 3, #3002).

THE EXIT-CODE-TRAP CLASS (bit the coordinator 4x this run): a backgrounded
`gate_and_push.sh > log; echo EXIT=$?` reports the WRAPPER's exit (the
tail/echo), not the gate's. The gate's real verdict is smeared across (process
$?, banner greps, a status file) and they can disagree:

  - #3002: rc=0 (PASS) over 31% coverage — 11,326 tests never ran.
  - #2633: rc=0 under a "shard crash / undercount" banner — gate_full_cycle
    distrusts rc=0+undercount-banner (Plan-A invariant 1.19) and retries.
  - The run-2026-08-08 exit-code-trap: `echo EXIT=$?` after a backgrounded
    pipeline read the wrapper's exit, not the gate's, four times.

This module writes ONE authoritative gate-verdict.json on every gate exit,
atomically, that Phase 3c consumers read instead of $?/greps.

PHASE 3c (consumers switched): gate_full_cycle.sh's run_until_trustworthy now
decides from the verdict (verdict==pass && trustworthy -> accept; refused/retry/
poisoned -> retry; fail -> bail), with the legacy $?/grep as a cross-check
assertion and fallback. gate_and_push.sh cross-checks the verdict before a push.
gate_wait.sh classifies from the verdict with log-grep fallback. The legacy
$?/banner/status-file outputs are UNCHANGED (back-compat) — the verdict is
primary, they are the belt-and-braces.

Stdlib only (no pytest). Comments cite issues (#3002, the exit-code-trap, etc).
See test_verdict.py for the load-bearing pins:
  - trustworthy=False iff pass+truncated (#3002 shape)
  - the verdict enum covers every gate.sh exit path (fail/retry/refused/poisoned/pass)
  - the file is valid JSON + present after a simulated mid-exit crash (atomic rename)
  - dual-consume equivalence: verdict decision == legacy $?/grep decision on all
    reachable gate states (the 0-disagreement proof)
"""

import argparse
import enum
import json
import os
import sys
import time

# Reuse the atomic writer from io.py (#3010 Phase 0) — its crash invariant
# (a simulated mid-write crash leaves NO partial target) is the foundation this
# module builds on: the authoritative verdict file must survive the exact
# mid-exit crash class that makes $? unreliable.
from gate import io

# The schema version every verdict carries. A consumer that reads a different
# schema version must refuse (the #3000 shape: a reader that proceeds on a
# document it does not understand is the defect). Bump on any breaking change.
SCHEMA = "bezel.gate-verdict.v1"

# Bump when the WRITE logic changes meaningfully. Lets a future consumer
# distinguish verdicts written by different writer generations.
RUNNER_VERSION = 1


class Verdict(enum.Enum):
    """The five possible gate outcomes, one per gate.sh exit path.

    Maps gate.sh's exit codes faithfully (verdict_from_rc):
      PASS     — exit 0, no truncation evidence. The gate ran to completion
                 and every phase passed.
      FAIL     — exit 1, a real regression (via fail(): configure/build/lit/
                 canary/shard failure, or a suspect that fails alone all 5
                 arbitration tries).
      RETRY    — exit 3, coverage short (#3002). The sharded pass reported
                 fewer gtests than the registry says exist — a truncated run
                 says nothing about the tree, and truncation is usually
                 transient (contention, a wedged shard). gate_full_cycle.sh
                 retries on exit 3.
      REFUSED  — exit 2 or 64, preflight refused. The gate did not even try:
                 exit 2 = heavy build/test processes already active (the
                 zero-builders cold-cache-flake defense) or not a git repo;
                 exit 64 = unrecognized MODE (the #3011 positive-list guard).
      POISONED — rc=0 but the undercount banner ("## GATE FAILURE: shard crash
                 / undercount") is present in the shard log. Plan-A invariant
                 1.19: gate_full_cycle distrusts rc=0+undercount-banner and
                 retries. This verdict IS that distrust, materialized — so a
                 Phase 3c consumer checking the verdict (not $? or a grep)
                 gets the right answer without re-implementing the distrust.
    """
    PASS = "pass"
    FAIL = "fail"
    RETRY = "retry"
    REFUSED = "refused"
    POISONED = "poisoned"


def verdict_from_rc(rc, truncated=False):
    """Map a gate.sh exit code to a Verdict.

    The exit-code taxonomy (gate.sh):
      0   GATE PASS (or POISONED if truncation evidence is present — #3002)
      1   real failure (via fail(): configure/build/lit/canary/shard)
      2   preflight refused (zero-builders rule, gate.sh:113; or not a repo, :71)
      3   coverage short (#3002 — transient truncation, gate_full_cycle retries)
      64  preflight refused (unrecognized mode, gate.sh:95 — #3011)

    `truncated` is the integrity signal (the undercount banner was present, or
    the coverage floor reported a short). It only affects the rc==0 case: a
    pass over a truncated run is POISONED, not PASS. For all other exit codes
    the verdict is determined by the exit code alone (a FAIL is a FAIL whether
    or not there was also truncation).

    Any unrecognized rc is mapped to FAIL (fail-closed). Per the exit-code-trap
    lesson: "exit 0 is not an oracle" — and by symmetry, an UNKNOWN exit is
    never a pass. A consumer must never see PASS for an exit code this function
    does not know about.
    """
    if rc == 0:
        return Verdict.POISONED if truncated else Verdict.PASS
    if rc == 1:
        return Verdict.FAIL
    if rc == 2:
        return Verdict.REFUSED
    if rc == 3:
        return Verdict.RETRY
    if rc == 64:
        return Verdict.REFUSED
    return Verdict.FAIL


def is_trustworthy(verdict, truncated):
    """True iff the verdict can be trusted as a pass.

    False iff verdict==pass but integrity says truncated — the #3002 shape,
    where the gate exits 0 (pass) over a run that was actually truncated. The
    POISONED verdict handles the DETECTED case (rc=0 + undercount banner ->
    poisoned); this flag is the independent safety net for the residual risk
    that the verdict computation said 'pass' when integrity says otherwise.

    A consumer should check BOTH `verdict == "pass"` AND `trustworthy` before
    acting on a pass. A non-pass verdict (fail/retry/refused/poisoned) is
    always trustworthy in its own assessment — it honestly reports its outcome;
    `trustworthy` specifically guards the pass case.

    per #3002: the coverage floor (exit 3 -> retry) catches most truncation
    structurally. This flag is the defense-in-depth on top of that: if the
    floor somehow let a truncated pass through (e.g. the registry was
    unreadable and the floor was not enforced), trustworthy catches it here.
    """
    return not (verdict == Verdict.PASS and truncated)


def build_verdict_obj(rc, truncated=False, head="", mode="", oracle="",
                      started_epoch=0, total_s=0.0, failed_phase=None,
                      undercount_banner=False,
                      shard_count=0, shard_reported_s=0,
                      coverage_gtests=None, coverage_littests=None,
                      expected_gtests=None,
                      asan_errors=None, arbitration_suspects=None,
                      repo=""):
    """Build the full verdict schema dict from gate context.

    This is the pure construction helper: it takes the raw fields gate.sh
    harvests and returns the canonical schema object. Separated from
    write_verdict so the construction is unit-testable without touching the
    filesystem.

    `truncated` is the composite integrity flag (undercount_banner OR coverage
    short(rc==3)). `undercount_banner` is the specific "## GATE FAILURE: shard
    crash / undercount" marker from run_parallel.sh, kept as a separate
    integrity field so a consumer can see WHY truncated is true.

    Coverage fields default to None (not 0) when not measured — per #3010's
    lesson, an unmeasured number must emit null, never 0, so a genuine zero is
    distinguishable from a harvest failure.

    `repo` (fix #3413) is the absolute path of the WORKING TREE the gate
    validated. A verdict carried `head` but never said WHICH tree that head was
    read from, so a gate that silently validated another checkout produced a
    verdict indistinguishable from a correct one. Run 4 cycle 4 (2026-08-27)
    reported both oracles green and pushed e9bd762 while this file recorded
    head=d34c8b4 — the previous day's commit on another branch, 56 files /
    +7775 lines apart — because gate_full_cycle.sh cd'd to a hardcoded path.
    With `repo` present the verdict names its own subject, and the push
    boundary can compare it against the tree it is about to push.
    """
    v = verdict_from_rc(rc, truncated)
    trustworthy = is_trustworthy(v, truncated)
    return {
        "schema": SCHEMA,
        "verdict": v.value,
        "trustworthy": trustworthy,
        "head": head,
        "repo": repo,
        "mode": mode,
        "oracle": oracle,
        "started_epoch": started_epoch,
        "total_s": total_s,
        "failed_phase": failed_phase,
        "evidence": {
            "shard": {
                "count": shard_count,
                "reported_s": shard_reported_s,
            },
            "coverage": {
                "gtests": coverage_gtests,
                "littests": coverage_littests,
                "expected_gtests": expected_gtests,
            },
            "asan": {
                "errors": asan_errors,
            },
            "arbitration": {
                "suspects": arbitration_suspects,
            },
        },
        "integrity": {
            "truncated": truncated,
            "undercount_banner": undercount_banner,
        },
        "runner_version": RUNNER_VERSION,
    }


def write_verdict(path, obj):
    """Write the verdict dict to `path` atomically (temp+fsync+os.replace).

    Delegates to io.atomic_json_write (#3010 Phase 0) so the crash invariant
    holds: a mid-exit crash leaves NO partial target. The file is either the
    old bytes or the new bytes, never garbage — which is the whole point of an
    authoritative verdict file that survives the exit-code-trap.
    """
    io.atomic_json_write(path, obj)


def read_verdict(path):
    """Read a verdict file. Returns the parsed dict, or None if missing/
    unparseable. A reader must never crash on a half-written document (the
    atomic write makes that impossible in practice, but graceful degradation
    is the #3003 shape: plausible-looking garbage must not take the consumer
    down — return None and let the caller fall back to $?/banners)."""
    try:
        with open(path) as f:
            obj = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(obj, dict):
        return None
    return obj


# The truncation marker run_parallel.sh emits on a shard crash / undercount
# (run_parallel.sh:300 — "## GATE FAILURE: shard crash / undercount — result is
# NOT trustworthy"). Kept as a literal so the detection is grep-equivalent but
# does not spawn a process or interpret regex metachars. Three callers used to
# grep this exact wording independently (gate.sh's gate_write_verdict,
# gate_full_cycle.sh's _legacy_rc_decision, and the bash dual-run assertion);
# per #3010 Wave 3 verdict.py now OWNS it so the wording lives in one place —
# the module that already owns the trustworthy=False-iff-pass+truncated pin.
UNDERCOUNT_BANNER = "shard crash / undercount"


def detect_truncation(shard_log, rc):
    """Read the shard log + the exit code; return (truncated, undercount_banner).

    This is the detection gate.sh's gate_write_verdict used to perform in bash
    (`grep -q 'shard crash / undercount' "$S/gate_shard.log"` plus the rc==3
    coverage-short signal), now owned by Python per the #3010 Wave 3 migration.
    The banner grep becomes an `in` check on the log content — one authority for the
    wording instead of a fragile bash re-implementation of run_parallel's banner.

    `undercount_banner` is the specific marker (the literal was present in a
    readable shard log). `truncated` is the composite integrity flag
    (undercount_banner OR the rc==3 coverage short, #3002). The composite only
    turns an rc==0 PASS into POISONED (verdict_from_rc), but is recorded in the
    integrity block for ALL exit codes so a consumer sees the full picture.

    A missing/unreadable shard log means no banner was seen — undercount_banner
    stays False (mirrors bash's `[ -r "$S/gate_shard.log" ] &&` readability
    guard). per the exit-code-trap / #3001: rc==0 over an undercount banner is
    POISONED, and rc==3 (coverage short) is RETRY.
    """
    undercount_banner = False
    if shard_log:
        try:
            with open(shard_log, errors="replace") as f:
                content = f.read()
        except OSError:
            content = ""
        undercount_banner = UNDERCOUNT_BANNER in content
    truncated = undercount_banner or (rc == 3)
    return truncated, undercount_banner


# ---------------------------------------------------------------------------
# CLI — gate.sh calls this to write the verdict on every exit.
#
# All fields are optional except --path and --rc: a minimal early-exit verdict
# (preflight refused before any context was gathered) passes only those two;
# gate_finish passes the full context. Defaults keep the JSON well-formed
# regardless of how little is known at the exit point.
# ---------------------------------------------------------------------------

def _main_write(argv):
    p = argparse.ArgumentParser(
        prog="verdict.py write",
        description="Write an authoritative gate-verdict.json (Phase 3, #3002).",
    )
    p.add_argument("--path", required=True,
                   help="output path (default gate.sh uses $S/gate-verdict.json)")
    p.add_argument("--rc", type=int, required=True,
                   help="gate.sh exit code (0/1/2/3/64)")
    p.add_argument("--truncated", action="store_true", default=False,
                   help="truncation evidence present (undercount banner / coverage short)")
    p.add_argument("--undercount-banner", action="store_true", default=False,
                   help="the 'shard crash / undercount' marker was in the shard log")
    p.add_argument("--head", default="", help="git HEAD sha")
    p.add_argument("--repo", default="",
                   help="absolute path of the working tree that was gated "
                        "(fix #3413 — a verdict must name its own subject)")
    p.add_argument("--mode", default="", help="gate mode (default/--full/--quick/--no-asan)")
    p.add_argument("--oracle", default="", help="oracle (plain/asan)")
    p.add_argument("--started-epoch", type=int, default=0, help="gate start epoch seconds")
    p.add_argument("--total-s", type=float, default=0.0, help="total gate wall seconds")
    p.add_argument("--failed-phase", default=None, help="which phase failed (or null)")
    p.add_argument("--shard-count", type=int, default=0, help="number of shards")
    p.add_argument("--shard-reported-s", type=int, default=0, help="shard aggregate wall seconds")
    p.add_argument("--coverage-gtests", type=int, default=None, help="gtest count that ran")
    p.add_argument("--coverage-littests", type=int, default=None, help="lit count that ran")
    p.add_argument("--expected-gtests", type=int, default=None, help="registry-expected gtest count")
    p.add_argument("--asan-errors", type=int, default=None, help="ASan report count")
    p.add_argument("--arbitration-suspects", type=int, default=None, help="arbitration suspect count")
    args = p.parse_args(argv)

    obj = build_verdict_obj(
        rc=args.rc,
        truncated=args.truncated,
        head=args.head,
        repo=args.repo,
        mode=args.mode,
        oracle=args.oracle,
        started_epoch=args.started_epoch,
        total_s=args.total_s,
        failed_phase=args.failed_phase,
        undercount_banner=args.undercount_banner,
        shard_count=args.shard_count,
        shard_reported_s=args.shard_reported_s,
        coverage_gtests=args.coverage_gtests,
        coverage_littests=args.coverage_littests,
        expected_gtests=args.expected_gtests,
        asan_errors=args.asan_errors,
        arbitration_suspects=args.arbitration_suspects,
    )
    write_verdict(args.path, obj)
    return 0


def _optional_int(s):
    """Parse a CLI value as int, treating empty/None as 'unset' (-> None).

    Mirrors the bash `[ -n "${VAR+x}" ]` conditional guards gate.sh's old
    gate_write_verdict used for the harvested coverage/asan/arbitration fields:
    a field the caller did not measure (unset -> "") emits None in the JSON,
    NEVER 0, so a harvest failure and a genuine zero stay distinguishable
    (per #3010). A set field carries its integer value.

    This is the move that collapses the 12 conditional arg guards: gate.sh now
    passes `${VAR:-}` unconditionally and Python owns the set-vs-unset decision,
    instead of bash branching per field.
    """
    if s is None or s == "":
        return None
    return int(s)


def _main_write_from_state(argv):
    """`write_from_state` subcommand — gate.sh's entry point (#3010 Wave 3).

    Migrates gate.sh's gate_write_verdict (the truncation banner grep + the 12
    conditional `[ -n "${X+x}" ]` arg guards) INTO Python. The caller passes the
    RAW gate state (rc, the shard-log path, and the harvested fields with
    `${VAR:-}` defaults); this subcommand:

      (a) reads the shard log ITSELF and computes truncation — the banner grep
          is now `detect_truncation`'s `in` check (no process spawn, no regex),
          one authority for run_parallel's banner wording instead of a bash
          re-implementation;
      (b) builds the verdict object from kwargs, normalizing empty coverage
          fields to None via `_optional_int` (the set-vs-unset conditionality is
          now Python's, not bash's per-field `[ -n ]` branch).

    The 12 conditional guards collapse to: always pass the flag, let the default
    + `_optional_int` handle 'unset'. Best-effort remains the caller's `|| true`
    (preserved verbatim) — a verdict-write hiccup never changes the gate's exit
    code. Dual-run equivalence with the old `write` + bash arg-assembly path is
    pinned by DualRunWriteFromStateTest over a matrix of reachable gate states.
    """
    p = argparse.ArgumentParser(
        prog="verdict.py write_from_state",
        description="Write gate-verdict.json from raw gate state, computing "
                    "truncation from the shard log (#3010 Wave 3).",
    )
    p.add_argument("--path", required=True,
                   help="output path (gate.sh uses $GATE_VERDICT_FILE)")
    p.add_argument("--rc", type=int, required=True,
                   help="gate.sh exit code (0/1/2/3/64)")
    p.add_argument("--shard-log", default=None,
                   help="shard log path; read for the undercount banner "
                        "(missing/unreadable => no banner seen)")
    p.add_argument("--head", default="", help="git HEAD sha")
    p.add_argument("--repo", default="",
                   help="absolute path of the working tree that was gated "
                        "(fix #3413 — a verdict must name its own subject)")
    p.add_argument("--mode", default="",
                   help="gate mode (default/--full/--quick/--no-asan). "
                        "Caller uses the --mode=VALUE form so a '--full' value "
                        "is not mistaken for an unknown flag (#3010 comment).")
    p.add_argument("--oracle", default="", help="oracle (plain/asan)")
    p.add_argument("--started-epoch", type=int, default=0, help="gate start epoch seconds")
    p.add_argument("--total-s", type=float, default=0.0, help="total gate wall seconds")
    p.add_argument("--failed-phase", default=None,
                   help="which phase failed (empty => None)")
    p.add_argument("--shard-count", type=int, default=0, help="number of shards")
    p.add_argument("--shard-reported-s", type=int, default=0, help="shard aggregate wall seconds")
    p.add_argument("--coverage-gtests", default="",
                   help="gtest count that ran (empty => unmeasured => null)")
    p.add_argument("--coverage-littests", default="",
                   help="lit count that ran (empty => unmeasured => null)")
    p.add_argument("--expected-gtests", default="",
                   help="registry-expected gtest count (empty => unmeasured => null)")
    p.add_argument("--asan-errors", default="",
                   help="ASan report count (empty => unmeasured => null)")
    p.add_argument("--arbitration-suspects", default="",
                   help="arbitration suspect count (empty => unmeasured => null)")
    args = p.parse_args(argv)

    truncated, undercount_banner = detect_truncation(args.shard_log, args.rc)
    failed_phase = args.failed_phase if args.failed_phase else None

    obj = build_verdict_obj(
        rc=args.rc,
        truncated=truncated,
        head=args.head,
        repo=args.repo,
        mode=args.mode,
        oracle=args.oracle,
        started_epoch=args.started_epoch,
        total_s=args.total_s,
        failed_phase=failed_phase,
        undercount_banner=undercount_banner,
        shard_count=args.shard_count,
        shard_reported_s=args.shard_reported_s,
        coverage_gtests=_optional_int(args.coverage_gtests),
        coverage_littests=_optional_int(args.coverage_littests),
        expected_gtests=_optional_int(args.expected_gtests),
        asan_errors=_optional_int(args.asan_errors),
        arbitration_suspects=_optional_int(args.arbitration_suspects),
    )
    write_verdict(args.path, obj)
    return 0


def _main_read(argv):
    """`read` subcommand — the consumer-side entry point (Phase 3c, #3002).

    THE EXIT-CODE-TRAP, RECIPROCAL: gate.sh writes the verdict on every exit so
    a consumer does not have to trust $?/banner greps. This is how a consumer
    READS it back. The exit code is the canonical green-oracle question —
    "is this a trustworthy pass?" — so a consumer can:

        if PYTHONPATH=scripts python3 -m gate.verdict read --path "$VF"; then
            # verdict == pass && trustworthy -> accept
        fi

    Exit codes:
      0  verdict is pass AND trustworthy (a green gate you can act on).
      1  verdict is present but NOT a trustworthy pass (fail/retry/refused/
         poisoned — the consumer maps these to retry/bail per its own policy).
      2  verdict file missing, unparseable, stale (head mismatch), or the
         requested field is absent. The consumer FALLS BACK to $?/greps
         (back-compat; the verdict writer is best-effort, never load-bearing
         on its own — per #3002's additive-then-switch discipline).

    `--field NAME` prints a single top-level field (verdict/trustworthy/head/
    mode/oracle/...) to stdout for diagnostic display; it does NOT change the
    exit code (which is always the pass-and-trustworthy predicate). This lets a
    consumer fetch the verdict value for a retry/bail branch while still using
    the exit code for the accept check.

    `--expect-head SHA` refuses (exit 2) if the verdict's head != SHA. This is
    the disambiguation a poller uses to reject a STALE or CONCURRENT gate's
    verdict on the shared default path (#3000 shape: a reader that proceeds on
    a document not its own is the defect).

    `--expect-repo PATH` is the same disambiguation for the TREE (fix #3413).
    ⚠ Both are deliberately EXIT 2 ("not my document"), not exit 1 ("not a
    pass"), because a poller that owns neither must fall back rather than read
    a stranger's outcome. A consumer that can DISTINGUISH the two — the push
    boundary — must NOT use this flag for the decision: exit 2 also means
    "missing", and gate_and_push.sh's missing-verdict fallback is to trust $?
    and push. That collapse is how Run 4 cycle 4 pushed e9bd762 over a verdict
    naming d34c8b4: the stale verdict was reported as "absent" and the push
    proceeded on a note. Read `--field head` / `--field repo` directly (exit 2
    then means genuinely absent) and refuse on a value that disagrees.
    """
    p = argparse.ArgumentParser(
        prog="verdict.py read",
        description="Read an authoritative gate-verdict.json (Phase 3c, #3002). "
                    "Exit 0 iff verdict==pass && trustworthy; 1 iff present but "
                    "not a trustworthy pass; 2 iff missing/unparseable/stale.",
    )
    p.add_argument("--path", required=True, help="verdict file path to read")
    p.add_argument("--field", default=None,
                   help="print a single top-level field (verdict/trustworthy/"
                        "head/mode/oracle/...) to stdout; exit code is unchanged")
    p.add_argument("--expect-head", default=None,
                   help="refuse (exit 2) if the verdict head != this sha "
                        "(disambiguates a stale/concurrent gate's verdict)")
    p.add_argument("--expect-repo", default=None,
                   help="refuse (exit 2) if the verdict repo != this path "
                        "(fix #3413 — disambiguates a verdict written by a "
                        "gate that validated another working tree)")
    args = p.parse_args(argv)

    obj = read_verdict(args.path)
    if obj is None:
        # Missing or unparseable — consumer falls back to the legacy $?/grep
        # path (back-compat; the verdict writer is best-effort, per #3002).
        return 2

    if obj.get("schema") != SCHEMA:
        # #3000 shape: a reader that proceeds on a document it does not
        # understand is the defect. Every verdict this module writes carries
        # `schema: SCHEMA` (build_verdict_obj); a document with a different or
        # absent schema is not one this reader can interpret, so refuse (rc=2,
        # same as missing/unparseable -> the consumer falls back) rather than
        # guess at unknown fields. This is the read-side check the module
        # docstring promises but did not enforce.
        sys.stderr.write(
            "verdict read: schema mismatch (got {!r}, expected {!r}) — refusing "
            "an unknown verdict document at {}\n".format(
                obj.get("schema"), SCHEMA, args.path))
        return 2

    if args.expect_head is not None:
        head = obj.get("head", "")
        if head != args.expect_head:
            # Stale: another gate's verdict, or a back-to-back run overwrote the
            # shared path. Refuse rather than acting on the wrong gate's outcome
            # (the #3000 shape: a reader that proceeds on a document it does not
            # own is the defect).
            sys.stderr.write(
                "verdict read: head mismatch (got {!r}, expected {!r}) — refusing "
                "a stale/concurrent verdict at {}\n".format(
                    head[:12], args.expect_head[:12], args.path))
            return 2

    if args.expect_repo is not None:
        # fix #3413: the same staleness question as --expect-head, asked of the
        # TREE. A verdict written before this field existed has no "repo" key;
        # treat that as unanswerable and refuse (exit 2 -> the consumer falls
        # back) rather than assuming it matches — an absent field is not
        # evidence of agreement (the "not X => assume Z" category defect).
        repo = obj.get("repo")
        if repo != args.expect_repo:
            sys.stderr.write(
                "verdict read: repo mismatch (got {!r}, expected {!r}) — refusing "
                "a verdict that describes another working tree at {}\n".format(
                    repo, args.expect_repo, args.path))
            return 2

    if args.field is not None:
        if args.field not in obj:
            sys.stderr.write(
                "verdict read: no such field {!r} in {}\n".format(
                    args.field, args.path))
            return 2
        val = obj[args.field]
        # Booleans print as JSON lowercases (true/false), not Python title-case,
        # so a shell consumer can test [ "$val" = true ].
        if isinstance(val, bool):
            print("true" if val else "false")
        elif val is None:
            print("null")
        else:
            print(val)

    # The exit code is always the trustworthy-pass predicate, regardless of
    # --field (the field is for display; the exit code is the decision). per the
    # exit-code-trap: "exit 0 is not an oracle" — and a non-pass verdict is
    # honestly non-zero here.
    v = obj.get("verdict")
    tw = obj.get("trustworthy")
    if v == Verdict.PASS.value and tw is True:
        return 0
    return 1


def main(argv):
    if len(argv) >= 2 and argv[1] == "write":
        return _main_write(argv[2:])
    if len(argv) >= 2 and argv[1] == "write_from_state":
        return _main_write_from_state(argv[2:])
    if len(argv) >= 2 and argv[1] == "read":
        return _main_read(argv[2:])
    sys.stderr.write(
        "usage: verdict.py {write|write_from_state|read} --path PATH [options]\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
