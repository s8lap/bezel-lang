#!/usr/bin/env bash
# sweep_two_shas.sh <shaA> <shaB> [<test-sources-ref>=shaA]
#
# Purpose: find Bezel programs embedded in tests/**/*.cpp whose accept/reject outcome or first
# diagnostic differs between bezelc built at shaA and bezelc built at shaB. The .bzl-only sweep
# misses programs embedded in C++ tests (batch 31 missed generic_iface_recursive_property_5343 for
# this reason); this tool finds them mechanically (about 28.5k programs).
#
# Prerequisites:
#   - git, bash, python3, and a worktree WT (default: the git toplevel of the current directory)
#     with a configured in-tree build/ dir (scripts/lane_setup.sh). WT is used ONLY to build bezelc.
#   - WT must be clean (git status empty) and must not be used by another lane or build, because
#     this script switches it to the detached sha when a cached binary is missing and restores it
#     (also on failure or SIGINT/SIGTERM during the build), but WT must still be a dedicated clean worktree.
#   - Cached binaries $SCRATCH/bezelc-<sha8> are reused; a missing one is built through
#     scripts/agent_check.sh --probe (bezelc only, via dev_heavy).
#
# Environment:
#   WT            build worktree (default: git toplevel of the current directory)
#   SWEEP_SCRATCH scratch root (default /var/tmp/bezel-scratch/sweep); holds bezelc-<sha8> and trivial.bzl
#   OUT           output dir (default $SWEEP_SCRATCH/run-<A>-<B>)
#
# Output: $OUT/diff.tsv (one row per program whose result differs between the two binaries).
#   Columns: class, name, file, line, test, kind, rcA, rcB, diagA, diagB, test_file_changed
#   test_file_changed = yes if the row's test file is in `git diff --name-only shaA shaB -- tests`,
#   no if not, unknown if git cannot resolve the range.
# Classes:
#   a  accept -> reject
#   b  reject -> accept
#   c  diagnostic text changed (same accept/reject outcome)
#   d  crash or timeout change
#
# Compiles use `-c`, a 60 s timeout, and 8 workers. Programs needing imports are ignored (identical
# on both sides). Python runs with `python3 -I`.
set -euo pipefail

SD=$(cd "$(dirname "$0")" && pwd)
WT=${WT:-$(git rev-parse --show-toplevel)}
S=${SWEEP_SCRATCH:-/var/tmp/bezel-scratch/sweep}
mkdir -p "$S"; S=$(cd "$S" && pwd)
A=$(git -C "$WT" rev-parse --short=8 "$1"); B=$(git -C "$WT" rev-parse --short=8 "$2")
AF=$(git -C "$WT" rev-parse "$1"); BF=$(git -C "$WT" rev-parse "$2")
REF=${3:-$A}
OUT=${OUT:-$S/run-$A-$B}; mkdir -p "$OUT"

if [ ! -f "$S/trivial.bzl" ]; then
  printf 'module Probe;\n' > "$S/trivial.bzl"
fi

restore_wt() {  # EXIT/INT/TERM handler set by build(); restores WT, clears its traps, never fails
  trap - EXIT INT TERM
  git -C "$WT" switch "${restore_args[@]}" >/dev/null 2>&1 \
    || echo "WARNING: could not restore $WT (git switch ${restore_args[*]})" >&2
  return 0
}

build() {  # $1 sha
  [ -x "$S/bezelc-$1" ] && { echo "cached bezelc-$1"; return; }
  cd "$WT"; [ -z "$(git status --short)" ] || { echo "WT dirty" >&2; exit 2; }
  # Record the original position before switching: a branch name, or the full sha if detached.
  if [ "$(git rev-parse --abbrev-ref HEAD)" = HEAD ]; then
    restore_args=(--detach "$(git rev-parse HEAD)")
  else
    restore_args=("$(git rev-parse --abbrev-ref HEAD)")
  fi
  trap restore_wt EXIT; trap 'restore_wt; exit 130' INT; trap 'restore_wt; exit 143' TERM
  git switch --detach "$1" >/dev/null
  echo "building at $(git rev-parse HEAD)"; git diff "$1" --stat -- src | wc -l
  scripts/agent_check.sh --probe "$S/trivial.bzl" || { echo "probe build failed at $1" >&2; exit 3; }
  cp build/bezelc "$S/bezelc-$1"
  restore_wt
  sha256sum "$S/bezelc-$1"
}
build "$A"; build "$B"
if [ ! -d "$OUT/progs" ]; then python3 -I "$SD/extract_embedded.py" "$WT" "$OUT/progs" "$REF"; mv "$OUT/progs/index.tsv" "$OUT/progs/stats.txt" "$OUT/" ; fi
for X in $A $B; do [ -s "$OUT/res-$X.tsv" ] || python3 -I "$SD/run_compile.py" "$S/bezelc-$X" "$OUT/progs" "$OUT/res-$X.tsv" "$OUT/o-$X"; done
python3 -I "$SD/diff_results.py" "$OUT/res-$A.tsv" "$OUT/res-$B.tsv" "$OUT/index.tsv" "$WT" "$AF" "$BF" > "$OUT/diff.tsv"
wc -l "$OUT/diff.tsv"
