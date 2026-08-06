#!/usr/bin/env bash
# Run the integration gate and push main ONLY on a trustworthy PASS. One command,
# one completion signal — the blessed way to land a batch without ad-hoc
# pgrep/log waiters (which have self-matched and deadlocked; see the
# build-test-efficiency notes). Backgroundable:
#   nohup scripts/gate_and_push.sh > /var/tmp/bezel-scratch/gateN.log 2>&1 &
# and rely on the single completion notification.
#
# fix #2917 (follow-on to #2633): THIS SCRIPT NOW GATES ON BOTH ORACLES.
#
# It used to call `scripts/gate.sh "$@"`, forwarding whatever single mode it was
# given — so `gate_and_push.sh --full` pushed on the ASan oracle ALONE. #2633
# measured why that is unsafe: `--full` is a DIFFERENT oracle, not a stronger
# one. ASan replaces the allocator and changes layout, so a wild read that
# FAULTS on the plain build can land in mapped memory and pass under ASan. Its
# evidence is one binary, one filter: 3 failures plain, 0 failures under ASan,
# where the underlying bug was a SIGSEGV on the default build. #2633 records the
# consequence — runs gated almost exclusively with `--full`, leaving that whole
# defect class "structurally invisible to the gate, however many times it ran".
#
# #2633 established the rule; nothing enforced it on the path that actually
# pushes. That is what this closes: the dual-oracle requirement now lives in the
# code that pushes, not in which script someone remembers to call. (Same
# treatment as the undercount banner, which became an acceptance CONDITION
# rather than a note — and caught a `GATE PASS` printed over 4,823 of ~15,300
# tests two gates later.)
#
# Usage:
#   scripts/gate_and_push.sh                 # BOTH oracles, then push  <- default
#   scripts/gate_and_push.sh --full          # accepted, NO-OP: the cycle already
#                                            #   runs ASan. Kept because it is the
#                                            #   spelling in CLAUDE.md and in every
#                                            #   habit; it now means "and plain too".
#   scripts/gate_and_push.sh -d DIR -t TAG -n N    # forwarded to gate_full_cycle.sh
#   scripts/gate_and_push.sh --single-oracle MODE  # ESCAPE HATCH, loud; see below
#
# REFUSED: --quick (shard-only). It is a diagnosis mode that skips coverage by
# design, so it can never be the basis of a push. Run it via scripts/gate.sh.
set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gate_and_push: not in a git repo" >&2; exit 2; }

CYCLE_ARGS=()
SINGLE_MODE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --full)
      # Historical spelling meaning "also run ASan". The cycle runs ASan
      # unconditionally, so this is now a no-op rather than a mode selector.
      # Accepted rather than rejected: every existing habit and CLAUDE.md spell
      # it this way, and silently UPGRADING them to both oracles is the safe
      # direction. Rejecting would only push callers back to bare gate.sh.
      echo "== gate_and_push: note — '--full' is a NO-OP here; this gate runs BOTH oracles (#2917) =="
      shift ;;
    --quick)
      echo "gate_and_push: REFUSING --quick." >&2
      echo "  --quick is shard-only: it skips coverage by design, so it cannot be the" >&2
      echo "  basis of a push (#2633/#2917). Use it for diagnosis via scripts/gate.sh," >&2
      echo "  then land the batch with a full scripts/gate_and_push.sh run." >&2
      exit 64 ;;
    --single-oracle)
      # Deliberate single-oracle gate, for bisection or for a tree where one mode
      # is known-broken for an unrelated reason. It still pushes — the point is
      # that the operator CHOSE it explicitly, rather than getting it by
      # forwarding a flag they did not think about, which is the exact failure
      # #2917 is about.
      [ $# -ge 2 ] || { echo "gate_and_push: --single-oracle needs a MODE (default|--full|--quick)" >&2; exit 64; }
      SINGLE_MODE="$2"; shift 2 ;;
    -d|-t|-n)
      [ $# -ge 2 ] || { echo "gate_and_push: $1 needs a value" >&2; exit 64; }
      CYCLE_ARGS+=("$1" "$2"); shift 2 ;;
    -h|--help)
      sed -n '2,38p' "$0"; exit 0 ;;
    *)
      echo "gate_and_push: unknown argument '$1' (see --help)" >&2; exit 64 ;;
  esac
done

if [ -n "$SINGLE_MODE" ]; then
  echo "== gate_and_push: SINGLE-ORACLE GATE ('$SINGLE_MODE') — NOT the full acceptance =="
  echo "==   #2633: the two oracles catch DIFFERENT defect classes; neither dominates.  =="
  echo "==   A push on one oracle is blind to the other's class. Chosen explicitly.     =="
  if [ "$SINGLE_MODE" = "default" ]; then set --; else set -- "$SINGLE_MODE"; fi
  GATE_CMD=(scripts/gate.sh "$@")
else
  GATE_CMD=(scripts/gate_full_cycle.sh "${CYCLE_ARGS[@]+"${CYCLE_ARGS[@]}"}")
fi

if "${GATE_CMD[@]}"; then
  echo "== gate_and_push: GATE PASS -> pushing =="
  if git push; then
    echo "== gate_and_push: PUSHED origin/main now $(git rev-parse --short origin/main) =="
    # Auto-GC on land: reap worktrees whose commits are now in origin/main
    # (branch refs kept; WIP/locked/current worktrees never touched), drop stale
    # ASan trees, and quarantine >48h root scratch. Best-effort — a GC hiccup
    # must never fail a push that already succeeded. Set BEZEL_NO_GC=1 to skip.
    if [ "${BEZEL_NO_GC:-0}" != 1 ] && [ -x scripts/gc_scratch.sh ]; then
      echo "== gate_and_push: auto-GC (landed worktrees + stale builds + root) =="
      scripts/gc_scratch.sh --landed-only --builds --root --apply || \
        echo "== gate_and_push: GC reported an issue (ignored; push already landed) =="
    fi
  else
    echo "== gate_and_push: PUSH FAILED (gate passed; resolve remote + push manually) =="
    exit 3
  fi
else
  rc=$?
  echo "== gate_and_push: GATE FAILED (rc=$rc) -> NOT pushing (main left with the batch committed; diagnose) =="
  exit 1
fi
