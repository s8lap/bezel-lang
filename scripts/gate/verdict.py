"""verdict.py — the authoritative gate verdict file (Phase 3-additive, #3002).

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
atomically, that Phase 3c consumers will read instead of $?/greps.

PHASE 3 IS ADDITIVE: the write lands here, but NO consumer switches yet (the
existing $?/banner/status-file outputs are unchanged — back-compat). The file
is observation-only until N gates prove it matches the gate's actual outcome,
then Phase 3c switches the readers (gate_full_cycle, gate_and_push, gate_wait,
dev agents).

Stdlib only (no pytest). Comments cite issues (#3002, the exit-code-trap, etc).
See test_verdict.py for the load-bearing pins:
  - trustworthy=False iff pass+truncated (#3002 shape)
  - the verdict enum covers every gate.sh exit path (fail/retry/refused/poisoned/pass)
  - the file is valid JSON + present after a simulated mid-exit crash (atomic rename)
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
                      asan_errors=None, arbitration_suspects=None):
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
    """
    v = verdict_from_rc(rc, truncated)
    trustworthy = is_trustworthy(v, truncated)
    return {
        "schema": SCHEMA,
        "verdict": v.value,
        "trustworthy": trustworthy,
        "head": head,
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


def main(argv):
    if len(argv) >= 2 and argv[1] == "write":
        return _main_write(argv[2:])
    # No subcommand: a future Phase 3c consumer will add a `read` subcommand.
    sys.stderr.write("usage: verdict.py write --path PATH --rc RC [options]\n")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
