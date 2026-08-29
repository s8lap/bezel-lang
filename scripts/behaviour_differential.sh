#!/usr/bin/env bash
#
# behaviour_differential.sh — the behaviour-preservation oracle for a PURE
# REFACTOR. Thin wrapper over scripts/behaviour_differential.py; the reasoning,
# the mismatch classes and the fail-closed guards are documented there.
#
# WHY A WRAPPER AT ALL. Two things a lane should not have to remember:
#
#   1. RESOURCE CAP. A full run is ~5,800 compile+link+exec pairs. Several lanes
#      may be running one at once, so it goes through the shared
#      bezel-heavy.slice cgroup (scripts/dev_heavy.sh) at PRODUCER weight — the
#      slice bounds TOTAL cpu+mem across every concurrent heavy op without
#      serializing them, exactly as CLAUDE.md's concurrency discipline requires.
#      Per-program parallelism inside the run defaults to the same
#      BEZEL_AOT_LINK_TOKENS knob the AOT test harness uses, so lowering that one
#      env var still bounds the whole machine.
#
#   2. --self-test IS FREE AND MUST NOT BE CAGED. It spawns no bezelc and takes
#      about a second; routing it through systemd-run would make it fail on a box
#      with no user systemd session, for no benefit. It is dispatched directly.
#
# USAGE
#   scripts/behaviour_differential.sh --self-test
#   scripts/behaviour_differential.sh --baseline <bezelc> --candidate <bezelc> \
#                                     [--corpus conformance|lit|both] [--jobs N] \
#                                     [--limit N] [--json FILE] [--keep]
#
# TYPICAL USE IN A DEDUP LANE
#   # 1. build the compiler as it is on main -> a baseline binary that KEEPS ITS
#   #    OWN BUILD TREE (bezelc links the libbezel_runtime.a its tree built;
#   #    a preserved binary is not a preserved experiment).
#   # 2. apply the refactor, rebuild -> the candidate.
#   # 3. scripts/behaviour_differential.sh --baseline BASE/build/bezelc \
#   #                                      --candidate ./build/bezelc
#   # A clean verdict is the evidence a pure refactor ships INSTEAD of a
#   # regression test, and the measured numbers belong in the commit body.
#
# Exit status: 0 only when every program behaved identically AND the run
# measured enough to say so. Non-zero on any confirmed mismatch and on any
# fail-closed guard.

set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "behaviour_differential: not inside a git worktree" >&2; exit 2; }
PY="$ROOT/scripts/behaviour_differential.py"
DH="$ROOT/scripts/dev_heavy.sh"

[ -f "$PY" ] || { echo "behaviour_differential: missing $PY" >&2; exit 2; }

# --self-test and --help need no compiler, no corpus and no cage.
for arg in "$@"; do
  case "$arg" in
    --self-test|-h|--help) exec python3 "$PY" "$@" ;;
  esac
done

# BEZEL_DIFF_NO_SLICE=1 for a box with no user systemd session (or when the
# caller is already inside a cage).
if [ -n "${BEZEL_DIFF_NO_SLICE:-}" ] || [ ! -x "$DH" ]; then
  exec python3 "$PY" "$@"
fi

# `dev_heavy build` and not `suite`: `suite` carries GATE priority
# (CPUWeight=10000), and a lane's differential must not outrank a running gate.
# The producer weight uses spare capacity in the shared slice.
exec "$DH" build python3 "$PY" "$@"
