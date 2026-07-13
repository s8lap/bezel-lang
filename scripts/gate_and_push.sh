#!/usr/bin/env bash
# Run the integration gate (scripts/gate.sh with the given args) and push main
# ONLY on GATE PASS. One command, one completion signal — the blessed way to land
# a batch without ad-hoc pgrep/log waiters (which have self-matched and
# deadlocked; see the build-test-efficiency notes). Backgroundable:
#   nohup scripts/gate_and_push.sh --full > /var/tmp/bezel-scratch/gateN.log 2>&1 &
# and rely on the single completion notification.
#
# Usage:
#   scripts/gate_and_push.sh            # default gate (build+shard+serial) then push
#   scripts/gate_and_push.sh --full     # + asan_sweep  (disposal/codegen/async/layout batches)
#   scripts/gate_and_push.sh --quick    # shard only    (sema-only batches)
set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gate_and_push: not in a git repo" >&2; exit 2; }
if scripts/gate.sh "$@"; then
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
  echo "== gate_and_push: GATE FAILED -> NOT pushing (main left with the batch committed; diagnose) =="
  exit 1
fi
