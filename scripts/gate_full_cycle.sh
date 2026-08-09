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

# Scratch dir + cycle-verdict path (matches gate.sh:76-77). The cycle writes its
# OWN verdict here (Phase 3c, per the exit-code-trap) so downstream consumers
# (gate_and_push.sh, gate_wait.sh) read the CYCLE outcome — not a per-attempt
# gate.sh verdict, which lives at a per-run path inside run_until_trustworthy
# (set via GATE_VERDICT_FILE, honored at gate.sh:77).
S=/var/tmp/bezel-scratch; mkdir -p "$S"
CYCLE_VERDICT_FILE="${GATE_VERDICT_FILE:-$S/gate-verdict.json}"

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

# _legacy_rc_decision — the decision the ORIGINAL $?/banner-grep logic makes.
# Kept as the CROSS-CHECK assertion (Phase 3c dual-consume) and the FALLBACK when
# the verdict file is missing (back-compat; the writer is best-effort, per
# #3002). per the exit-code-trap: in THIS foreground call $? is the gate's own
# exit (no pipeline/backgrounding), so it is reliable HERE — the trap bites the
# EXTERNAL observer (gate_and_push/gate_wait), not this inner loop.
_legacy_rc_decision() { # rc log -> echoes accept|retry|bail
  case "$1" in
    0)  if grep -q 'shard crash / undercount' "$2" 2>/dev/null; then echo retry; else echo accept; fi ;;
    2|3) echo retry ;;
    *)  echo bail ;;
  esac
}

# write_cycle_verdict — write the cycle-level authoritative verdict (Phase 3c,
# per the exit-code-trap). Called from the EXIT trap on EVERY cycle exit so a
# downstream consumer (gate_and_push.sh, gate_wait.sh) reads the CYCLE outcome
# from one file instead of the cycle's $? — which is reliable in-process but not
# across the backgrounding boundary that bit the coordinator 4x. Best-effort
# (`|| true`): a verdict-write hiccup must never change the exit code.
write_cycle_verdict() { # rc
  local rc="$1"
  local head; head="$(git rev-parse HEAD 2>/dev/null || echo '')"
  PYTHONPATH=scripts python3 -m gate.verdict write --path "$CYCLE_VERDICT_FILE" \
    --rc "$rc" --head="$head" --mode=cycle --oracle=dual 2>/dev/null || true
}
trap 'rc=$?; write_cycle_verdict "$rc" || true; exit $rc' EXIT

# Run one gate mode until it produces a TRUSTWORTHY result. Phase 3c (per the
# exit-code-trap): the decision now comes from the AUTHORITATIVE gate-verdict.json
# that gate.sh writes on every exit, not from $?/banner greps. The verdict
# materializes the rc==0+undercount distrust as a POISONED verdict (#3001), so
# this consumer checks one structured field instead of re-implementing the
# distrust grep. $?/grep is kept as a CROSS-CHECK assertion (dual-consume): the
# verdict and the legacy logic are derived from the same signals inside gate.sh,
# so disagreement means a bug or a race — scream LOUDLY and take the conservative
# (non-accept) decision. A missing verdict falls back to the legacy $?/grep.
#
# Decision mapping (verdict -> cycle action):
#   pass (+trustworthy) -> accept   rc==0, no undercount
#   refused             -> retry    rc==2: preflight, builders active (transient)
#   retry               -> retry    rc==3: coverage short (#3002)
#   poisoned            -> retry    rc==0 + undercount distrust (#3001)
#   fail                -> bail     rc==1: real regression
run_until_trustworthy() {
  local log="$1"; shift
  local mode_desc="${1:-plain}"; shift || true
  local i rc v_verdict v_rc dec old_dec reason
  for i in $(seq 1 "$MAX_ATTEMPTS"); do
    # BEZEL_GATE_CYCLE=1 tells gate.sh that a DUAL-oracle run is driving it, so
    # it suppresses its "this is one oracle, not an acceptance verdict" note
    # (#2917). GATE_VERDICT_FILE points THIS gate's verdict at a PER-ATTEMPT path
    # (the default is a SHARED file every gate overwrites — gate.sh:277), so the
    # consumer reads its OWN gate's verdict, not a stale/concurrent one.
    local vf="${log%.log}.attempt${i}.verdict.json"
    BEZEL_NO_GC=1 BEZEL_GATE_CYCLE=1 GATE_VERDICT_FILE="$vf" \
      scripts/gate.sh "$@" > "$log" 2>&1
    rc=$?

    # Read the verdict (exit 0 iff trustworthy pass; 1 iff present-not-pass;
    # 2 iff missing/unparseable — consumer falls back to $?).
    v_verdict="$(PYTHONPATH=scripts python3 -m gate.verdict read --path "$vf" --field verdict 2>/dev/null)"
    v_rc=$?

    # Map the verdict to a decision (empty = verdict absent -> fallback below).
    case "$v_verdict" in
      pass)     dec=accept ;;
      refused)  dec=retry ;;
      retry)    dec=retry ;;
      poisoned) dec=retry ;;
      fail)     dec=bail ;;
      *)        dec="" ;;
    esac

    # CROSS-CHECK (dual-consume): compute the legacy $?/grep decision and verify
    # the verdict agrees. Disagreement means a bug in the verdict write or a race
    # on the verdict file — scream LOUDLY and take the CONSERVATIVE non-accept
    # decision. per the exit-code-trap: a false accept is the worst outcome (55
    # pass-but-truncated gates over 24 days).
    old_dec="$(_legacy_rc_decision "$rc" "$log")"
    if [ -n "$dec" ] && [ "$dec" != "$old_dec" ]; then
      say "VERDICT CROSS-CHECK DISAGREEMENT: verdict=$v_verdict->$dec but \$?=$rc/grep->$old_dec (attempt $i)"
      say "  The verdict and legacy logic disagree — taking the conservative (non-accept) decision."
      if [ "$dec" = accept ] || [ "$old_dec" = accept ]; then
        dec=bail
      fi
    fi
    # FALLBACK: verdict missing/unparseable — use the legacy $?/grep decision.
    [ -z "$dec" ] && dec="$old_dec"

    # Act on the decision, preserving the per-case messages + evidence (#3001).
    case "$dec" in
      retry)
        # Determine the reason (verdict value, or rc in fallback mode) to keep
        # the original specific messages + sleep durations.
        reason="$v_verdict"
        [ -z "$reason" ] && case "$rc" in 2) reason=refused;; 3) reason=retry;; *) reason=poisoned;; esac
        if [ "$reason" = refused ]; then
          say "$mode_desc attempt $i: preflight refused (builders active) — waiting"
          sleep 45; continue
        fi
        # PRESERVE THE EVIDENCE (#3001). Every attempt writes to the same $log,
        # so each distrusted attempt was OVERWRITTEN by the next and only the
        # final (green) run survived — making an undercount undiagnosable: the
        # banner's own numbers (reported `ran` vs the registry total, which shard
        # dropped, whether a completion summary was missing) live in the
        # attempt's log and nowhere else. The 2026-08-07 close-out undercounted
        # three times in a row and left nothing to look at. Keep the canonical
        # path at the LAST attempt (callers grep it) and copy each aside.
        cp -f "$log" "${log%.log}.attempt${i}.log" 2>/dev/null || true
        if [ "$reason" = retry ]; then
          say "$mode_desc attempt $i: COVERAGE SHORT — $(grep -oE '[0-9]+ of [0-9]+ gtests ran' "$log" | tail -1) — retrying (log kept: ${log%.log}.attempt${i}.log)"
        else
          say "$mode_desc attempt $i: rc=$rc UNDERCOUNT banner present (verdict=$reason) — NOT trusted, retrying (log kept: ${log%.log}.attempt${i}.log)"
        fi
        sleep 60; continue ;;
      bail)
        say "$mode_desc attempt $i: REAL FAILURE rc=$rc verdict=${v_verdict:-n/a} — see $log"
        return "$rc" ;;
      accept)
        say "$mode_desc attempt $i: TRUSTWORTHY GREEN (verdict=${v_verdict:-pass}, rc=$rc) — $log"
        return 0 ;;
    esac
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

# fix #3011: THIS IS A HARD FAILURE, NOT A WARNING.
#
# It was `say "WARNING: …"` followed by an unconditional
# `say "cycle COMPLETE: both oracles trustworthy-green"; exit 0` — so a cycle
# that ran the PLAIN oracle twice announced dual-oracle green and pushed. The
# check already existed and already knew; it just had no teeth.
#
# The whole point of #2917/#2633 is that plain and ASan are DIFFERENT oracles,
# neither dominating. A cycle that silently ran one of them twice has not
# established the thing this script exists to establish, and "both oracles
# trustworthy-green" is then a false statement in the log that a human will
# later cite as evidence.
#
# gate.sh's mode allow-list (#3011) stops the typo that caused this. This is the
# belt to that braces: a POSITIVE allow-list missing a name is variant 1 of the
# category-defect taxonomy, so the downstream evidence check must stand on its
# own rather than trusting the upstream list to be complete.
#
# Phase 3c partial: the verdict carries an `oracle` field (gate.sh sets it from
# ASAN=1 when MODE=--full). Cross-check the last --full pass's verdict says
# oracle=asan. The log grep stays AUTHORITATIVE: it checks OBSERVED evidence
# (sanitizer output in the log), while the verdict's oracle is the INTENDED mode
# — the grep catches a mode that silently degraded even if the verdict says asan.
_fvf=$(ls -1 "$LOGDIR/${TAG}_full.attempt"*.verdict.json 2>/dev/null | sort -V | tail -1)
if [ -n "$_fvf" ]; then
  _ora="$(PYTHONPATH=scripts python3 -m gate.verdict read --path "$_fvf" --field oracle 2>/dev/null)"
  if [ "$_ora" != asan ]; then
    say "CYCLE FAILURE: the --full verdict says oracle='${_ora:-n/a}' (expected asan) — $_fvf"
    say "  Refusing to report dual-oracle green — the ASan oracle's verdict does not confirm it ran."
    exit 1
  fi
fi
if ! grep -qi 'asan\|sanitize' "$LOGDIR/${TAG}_full.log"; then
  say "CYCLE FAILURE: the --full log mentions no sanitizer — the ASan oracle did NOT run."
  say "  Refusing to report dual-oracle green for a cycle that ran one oracle twice."
  say "  Log: $LOGDIR/${TAG}_full.log"
  exit 1
fi

say "cycle COMPLETE: both oracles trustworthy-green"
exit 0
