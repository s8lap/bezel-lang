#!/usr/bin/env bash
# Trustworthy full gate cycle: run scripts/gate.sh in BOTH oracles — plain, then
# --full (generated-code ASan) — and only report success when each pass is
# genuinely trustworthy.
#
# Why this wrapper exists (two lessons that each cost a near-miss):
#
#   1. A `== GATE PASS ==` banner can sit on top of a run that never executed
#      most of the suite. On 2026-08-05 gate 7 printed PASS while its own log
#      said "shard crash / undercount": 15 of 16 codegen shards had died and
#      5434 of 15123 tests actually ran. rc==0 is NOT sufficient — the absence
#      of the undercount banner is part of the acceptance condition, and a run
#      that undercounts is RETRIED rather than believed.
#
#   2. `--full` is a DIFFERENT oracle, not a stronger one (#2633). ASan changes
#      program behaviour and makes some bad reads benign, so the plain gate
#      catches defects --full masks, and vice versa. Both must run.
#
# ⚠ gate.sh reads MODE="${1:-default}" — the FIRST argument only. Passing it a
# flag it does not implement (e.g. --clean-first) silently DISPLACES --full and
# runs the "ASan" half as a plain gate while printing the wrong mode. This
# script therefore passes gate.sh nothing but the modes gate.sh implements.
#
# Usage:
#   scripts/gate_full_cycle.sh [-d LOGDIR] [-t TAG] [-n MAX_ATTEMPTS]
#
#   -d LOGDIR   where the two logs land (default /var/tmp/bezel-scratch/gate-cycle)
#   -t TAG      prefix for this cycle's log names (default: the current HEAD sha)
#   -n N        max attempts per mode before giving up (default 40)
#
# Exit codes: 0 = both modes trustworthy-green. Non-zero = the gate's own rc for
# a real failure, or 99 if a mode never produced a trustworthy run.
#
# Environment: BEZEL_NO_GC=1 is set for the gate runs — gate_and_push.sh's
# post-push worktree reap is slow (~1 worktree / 14s) and has no business
# running inside a gate cycle. Reap separately at consolidation.

set -u

REPO=/home/stepan/w/code/bezel-lang
cd "$REPO" || exit 1

LOGDIR=/var/tmp/bezel-scratch/gate-cycle
TAG=""
MAX_ATTEMPTS=40

while [ $# -gt 0 ]; do
  case "$1" in
    -d) LOGDIR="$2"; shift 2 ;;
    -t) TAG="$2"; shift 2 ;;
    -n) MAX_ATTEMPTS="$2"; shift 2 ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "gate_full_cycle: unknown argument '$1'" >&2; exit 64 ;;
  esac
done

[ -n "$TAG" ] || TAG="$(git rev-parse --short HEAD 2>/dev/null || echo notag)"
mkdir -p "$LOGDIR" || exit 1
CYCLE_LOG="$LOGDIR/${TAG}_cycle.log"

say() { echo "[gate_full_cycle $(date '+%H:%M:%S')] $*" | tee -a "$CYCLE_LOG"; }

# Run one gate mode until it produces a TRUSTWORTHY result, i.e. rc==0 AND no
# undercount banner. rc==2 is gate.sh's "preflight refused: builders active" —
# that is a retry, not a failure. Any other non-zero rc is a REAL failure and is
# returned immediately: retrying a red gate just hides the regression.
run_until_trustworthy() {
  local log="$1"; shift
  local mode_desc="${1:-plain}"; shift || true
  local i rc
  for i in $(seq 1 "$MAX_ATTEMPTS"); do
    # BEZEL_GATE_CYCLE=1 tells gate.sh that a DUAL-oracle run is driving it, so
    # it suppresses its "this is one oracle, not an acceptance verdict" note
    # (#2917). The note exists for direct single-mode callers; inside the cycle
    # it would be false — both oracles are being run and this wrapper applies
    # the real acceptance condition.
    BEZEL_NO_GC=1 BEZEL_GATE_CYCLE=1 scripts/gate.sh "$@" > "$log" 2>&1
    rc=$?
    if [ "$rc" -eq 2 ]; then
      say "$mode_desc attempt $i: preflight refused (builders active) — waiting"
      sleep 45; continue
    fi
    if [ "$rc" -ne 0 ]; then
      say "$mode_desc attempt $i: REAL FAILURE rc=$rc — see $log"
      return "$rc"
    fi
    if grep -q 'shard crash / undercount' "$log"; then
      # PRESERVE THE EVIDENCE (#3001). Every attempt writes to the same $log, so
      # before this loop each distrusted attempt was OVERWRITTEN by the next one
      # and only the final (green) run survived. That made the undercount
      # undiagnosable: the banner's own numbers — reported `ran` vs the registry
      # total, which shard dropped, whether a completion summary was missing at
      # all — live in the attempt's log and nowhere else. The 2026-08-07
      # close-out undercounted three times in a row and left nothing to look at.
      # Keep the canonical path pointing at the LAST attempt (callers grep it),
      # and copy each distrusted one aside.
      cp -f "$log" "${log%.log}.attempt${i}.log" 2>/dev/null || true
      say "$mode_desc attempt $i: rc=0 but UNDERCOUNT banner present — NOT trusted, retrying (log kept: ${log%.log}.attempt${i}.log)"
      sleep 60; continue
    fi
    say "$mode_desc attempt $i: TRUSTWORTHY GREEN (rc=0, no undercount) — $log"
    return 0
  done
  say "$mode_desc: no trustworthy run in $MAX_ATTEMPTS attempts — giving up"
  return 99
}

say "cycle start at $(git rev-parse --short HEAD) — logs in $LOGDIR"

# STALENESS PREFLIGHT. A gate that validates a STALE compiler is the worst
# possible outcome: it reports green for code that was never built. This is not
# hypothetical — `cmake --build --target bezelc` has been observed printing
# "Built target bezelc" and NOT relinking, leaving a binary older than both its
# own object file and the sources (the #2426 unreliable-incremental family).
# During this run that cost a false "the fix does not work" finding and a
# lane's time before a binary mtime check settled it.
#
# gate.sh rebuilds before it tests, so this preflight is about the state we
# ENTER in: if the tree already carries a stale binary, say so loudly here
# rather than trusting the build phase to notice. Advisory, not fatal — the
# gate's own reconfigure+build is still the mechanism that fixes it; this only
# guarantees the condition is never silent.
if [ -x "$REPO/build/bezelc" ]; then
  newest_src="$(find "$REPO/src" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
                  -newer "$REPO/build/bezelc" -print -quit 2>/dev/null)"
  if [ -n "$newest_src" ]; then
    say "STALE: build/bezelc is older than $(basename "$newest_src") — the gate's"
    say "STALE: build phase must relink it. If a gate ever reports green while"
    say "STALE: this warning stands, do NOT believe the result (see #2426)."
  fi
fi

run_until_trustworthy "$LOGDIR/${TAG}_plain.log" "plain" || exit $?

# --full folds whole-suite generated-code AddressSanitizer into the same sharded
# pass (BEZEL_AOT_FORCE_SANITIZE=address). Confirm from the log that ASan was
# actually on: a mode that silently degraded to plain would report a green that
# means something else entirely.
run_until_trustworthy "$LOGDIR/${TAG}_full.log" "full(ASan)" --full || exit $?

if ! grep -qi 'asan\|sanitize' "$LOGDIR/${TAG}_full.log"; then
  say "WARNING: the --full log mentions no sanitizer — verify the mode took effect"
fi

say "cycle COMPLETE: both oracles trustworthy-green"
exit 0
