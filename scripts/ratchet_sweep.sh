#!/usr/bin/env bash
# ratchet_sweep.sh — run every build-free ratchet the gate runs, and fail closed.
#
# WHY THIS EXISTS. The pre-gate sweep used to be a shell loop pasted into
# CLAUDE.md with the check names spelled out. That list drifted: a lane working
# from a worktree branched before the list was last corrected reported "all
# seven ratchets PASS" while the gate ran thirteen, and the one it had not been
# told about (check_toolchain_skip_surface) was RED — a ~50 min cycle would have
# burned on a known-fail. A hardcoded list is a second copy of a question the
# gate already answers, which is the drift this project's bug-fixing plan calls
# its dominant defect generator. So: never enumerate the checks. DERIVE them
# from the gate, which is the single authority for what a cycle runs.
#
# Usage:  scripts/ratchet_sweep.sh          # run them all, print a verdict table
#         scripts/ratchet_sweep.sh --quiet  # only print failures
#
# Exit 0 = every ratchet PASS. Exit 1 = at least one RED (do NOT launch a gate).
# Build-free; takes ~70s. Safe to run while lanes are building.
set -uo pipefail

# Repo-relative, never an absolute checkout path: this script must work in any
# worktree, and a hardcoded path is what ratchet #3413 fails a cycle for.
REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1

GATE="scripts/gate_full_cycle.sh"
[ -r "$GATE" ] || { echo "ratchet_sweep: cannot read $GATE" >&2; exit 1; }

QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1

mapfile -t CHECKS < <(grep -oE 'scripts/check_[a-z_]+\.py' "$GATE" | sort -u)
if [ "${#CHECKS[@]}" -eq 0 ]; then
  # Fail closed: a sweep that found nothing to run must never look green.
  echo "ratchet_sweep: derived ZERO checks from $GATE — refusing to report green" >&2
  exit 1
fi

red=0
for s in "${CHECKS[@]}"; do
  name="$(basename "$s" .py)"
  if out="$(timeout 300 python3 "$s" --strict 2>&1)"; then
    [ "$QUIET" -eq 1 ] || printf '%-42s PASS\n' "$name"
  else
    printf '%-42s FAIL\n' "$name"
    printf '%s\n' "$out" | tail -6 | sed 's/^/      | /'
    red=$((red + 1))
  fi
done

echo "----"
if [ "$red" -eq 0 ]; then
  echo "ratchet_sweep: ${#CHECKS[@]}/${#CHECKS[@]} PASS"
else
  echo "ratchet_sweep: $red of ${#CHECKS[@]} RED — do NOT launch a gate"
fi
exit $(( red == 0 ? 0 : 1 ))
