#!/usr/bin/env bash
# gate_wait.sh — wait for a DETACHED gate to reach a terminal verdict, print it,
# and exit 0=pass / 1=fail / 2=timeout-or-badargs.
#
# WHY THIS EXISTS (do not hand-roll a waiter — this is the verified form):
# A setsid-detached `gate_and_push.sh` (detached so the harness's background-task
# reaper can't kill a ~15-min gate) is NOT a tracked task, so it emits no
# completion signal. The bridge is a tracked `run_in_background` waiter. Two
# hand-rolled-waiter bugs bit us on 2026-07-24 (every waiter hung, no
# notifications):
#   1. Polling gate.sh's status file failed — gate.sh wrote a shared/other path
#      (fixed since in gate.sh via ${GATE_STATUS_FILE:-...}, but path/staleness
#      failure modes remain for a status-file poller).
#   2. A `pgrep -f 'gate_and_push.sh --full'` liveness fallback SELF-MATCHED the
#      waiter's own command line and never fired.
# This watches the gate LOG LINE — the only fully-reliable, failure-mode-free
# signal — with no status-file and no pgrep. Invoke it as the body of a
# run_in_background call:
#     bash scripts/gate_wait.sh /path/to/gateN.log
# where gateN.log is the file the detached gate's stdout+stderr were redirected
# to, e.g.:
#     setsid bash -c 'scripts/gate_and_push.sh --full > /path/gateN.log 2>&1' &
#
# Usage: gate_wait.sh <gate-logfile> [max_iters=240] [sleep_s=15]   (default ~60min)
set -u

LOG="${1:?usage: gate_wait.sh <gate-logfile> [max_iters] [sleep_s]}"
MAX_ITERS="${2:-240}"
SLEEP_S="${3:-15}"

# Run from the repo root so PYTHONPATH=scripts finds gate.verdict (Phase 3c).
# Best-effort: if not in a worktree, the read CLI fails -> fall back to log greps.
cd "$(git rev-parse --show-toplevel 2>/dev/null)" 2>/dev/null || true
# Resolve PYTHONPATH from the script's OWN location (scripts/) so gate.verdict is
# found regardless of whether the cd above succeeded — otherwise a module-not-
# found exits 1, which the classifier below would read as "verdict says fail"
# instead of "verdict absent -> fall back to log greps" (#3000 shape).
_GATE_SCRIPTS="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd)"

# Phase 3c (per the exit-code-trap): the authoritative verdict file the gate
# writes on every exit. The shared default path is written by gate.sh (single-
# oracle) or gate_full_cycle's EXIT trap (dual-oracle cycle). Used as the PRIMARY
# classification source below; log markers are the cross-check/fallback.
S=/var/tmp/bezel-scratch
GATE_VERDICT="${GATE_VERDICT_FILE:-$S/gate-verdict.json}"
# The head this gate ran on — rejects a STALE verdict from a previous gate on the
# shared path (#3000 shape). Best-effort: if the repo context differs (worktree
# on another branch), the head won't match -> read CLI returns "stale" (exit 2)
# -> gate_wait falls back to log greps.
EXPECT_HEAD="$(git rev-parse HEAD 2>/dev/null || echo '')"

# Truly-terminal log lines (emitted only at the end, by gate.sh's gate_finish and
# by the gate_and_push wrapper). NOT '## GATE FAILURE:' — that shard-phase warning
# can precede arbitration and is not itself a verdict.
TERMINAL_RE='== GATE PASS|== GATE FAIL|GATE FAILED -> NOT pushing|PUSHED origin/main now'

n=0
until [ -f "$LOG" ] && grep -qE "$TERMINAL_RE" "$LOG" 2>/dev/null; do
  sleep "$SLEEP_S"
  n=$((n + 1))
  if [ "$n" -ge "$MAX_ITERS" ]; then
    echo "gate_wait: TIMEOUT after ~$((MAX_ITERS * SLEEP_S))s waiting on $LOG"
    tail -n 8 "$LOG" 2>/dev/null || echo "(log absent)"
    exit 2
  fi
done

echo "=== GATE TERMINAL ($LOG) ==="
grep -E 'coverage:|== GATE PASS|== GATE FAIL|GATE FAILED -> NOT pushing|PUSHED origin/main|failing tests' "$LOG" | tail -6
git rev-parse --is-inside-work-tree >/dev/null 2>&1 &&
  echo "origin/main: $(git ls-remote origin main 2>/dev/null | cut -c1-8)"

# Phase 3c (per the exit-code-trap): classify from the AUTHORITATIVE verdict file
# (primary), cross-checked against the log markers. The verdict is the structured
# source that survives the backgrounding boundary that bit the coordinator 4x;
# the log greps are the belt-and-braces fallback when the verdict is missing or
# stale. On disagreement (verdict says pass but log says fail), take the
# CONSERVATIVE fail — never report pass on a disagreement.
#
# Wave 3 (#3010): the conservative-non-accept reconciliation now lives in
# gate.cycle.cross_check. The verdict decision (accept iff read exit 0, bail iff
# present-not-pass, absent iff missing/stale) is cross-checked against the
# log-marker decision (bail on FAIL markers, accept on PASS/PUSHED markers, absent
# on neither). cross_check returns accept only when no dissent is present: on a
# disagreement it never accepts (the policy that stopped the 55-truncated-gates
# class); when one side is absent it defers to the other; when both are absent it
# bails (could-not-classify). The original 3-branch ladder is computed below as
# _legacy_exit and asserted equal (dual-consume 0-disagreement); keep for 1
# landing gate, then retire.
_v_args=(--path "$GATE_VERDICT")
[ -n "$EXPECT_HEAD" ] && _v_args+=(--expect-head "$EXPECT_HEAD")
PYTHONPATH="$_GATE_SCRIPTS" python3 -m gate.verdict read "${_v_args[@]}" 2>/dev/null; _v_rc=$?

# Verdict decision from the read-CLI exit (0 pass+trustworthy / 1 present-not-pass / 2 missing).
case "$_v_rc" in
  0) _vdec=accept ;;
  1) _vdec=bail ;;
  *) _vdec="" ;;
esac
# Log-marker decision: bail on FAIL, accept on PASS/PUSHED, else absent ("").
_ldec=""
if grep -qE '== GATE FAIL|GATE FAILED -> NOT pushing' "$LOG" 2>/dev/null; then
  _ldec=bail
elif grep -qE '== GATE PASS|PUSHED origin/main now' "$LOG" 2>/dev/null; then
  _ldec=accept
fi

# The reconciled decision via gate.cycle (primary). accept -> exit 0, else exit 1.
_gw_dec="$(PYTHONPATH="$_GATE_SCRIPTS" python3 -m gate.cycle cross-check \
    --verdict-decision "${_vdec}" --legacy-decision "${_ldec}" 2>/dev/null)"
_py_exit=0; [ "$_gw_dec" = accept ] || _py_exit=1

# Legacy 3-branch ladder (faithful original, :79-102) as the dual-consume
# CROSS-CHECK. Computes the same pass/fail from the raw read exit + log greps.
_has_fail=0; grep -qE '== GATE FAIL|GATE FAILED -> NOT pushing' "$LOG" 2>/dev/null && _has_fail=1
_has_pass=0; grep -qE '== GATE PASS|PUSHED origin/main now' "$LOG" 2>/dev/null && _has_pass=1
_legacy_exit=1
if [ "$_v_rc" -eq 0 ]; then
  [ "$_has_fail" -eq 1 ] && _legacy_exit=1 || _legacy_exit=0
elif [ "$_v_rc" -eq 1 ]; then
  _legacy_exit=1
else  # _v_rc == 2: verdict missing -> log decides; neither marker -> could-not-classify
  [ "$_has_pass" -eq 1 ] && _legacy_exit=0 || _legacy_exit=1
fi

# DUAL-CONSUME 0-disagreement assertion: the Python cross_check and the legacy
# ladder must agree on pass/fail. A divergence means the Python port is wrong.
if [ "$_py_exit" -ne "$_legacy_exit" ]; then
  echo "gate_wait: DUAL-CONSUME DISAGREEMENT — cross_check exit=$_py_exit but legacy ladder exit=$_legacy_exit (v_rc=$_v_rc vdec='${_vdec:-none}' ldec='${_ldec:-none}') in $LOG" >&2
  echo "  The Python port and the legacy ladder disagree; see test_cycle.CrossCheckTest." >&2
fi
# Surface the GATE's own internal disagreement (the case cross_check escalated
# on): verdict says pass but the log carries FAIL markers. This is the operator-
# facing diagnostic the original inline classifier printed; cross_check IS the
# reconciliation, but the message tells the operator WHY a pass became a fail.
if [ "$_v_rc" -eq 0 ] && [ "$_has_fail" -eq 1 ]; then
  echo "gate_wait: VERDICT CROSS-CHECK DISAGREEMENT — verdict says pass but log has FAIL markers in $LOG" >&2
fi
if [ "$_legacy_exit" -ne 0 ] && [ "$_v_rc" -eq 2 ] && [ "$_has_pass" -eq 0 ] && [ "$_has_fail" -eq 0 ]; then
  echo "gate_wait: reached a terminal marker but could not classify pass/fail — inspect $LOG"
fi
exit "$_legacy_exit"
