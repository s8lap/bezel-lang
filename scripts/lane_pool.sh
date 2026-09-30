#!/usr/bin/env bash
# lane_pool.sh — prepare and recycle a FIXED pool of lane worktrees.
#
# WHY. In the 2026-08-12 run every wave created fresh agent worktrees, so every
# lane paid a near-cold build (~9 min) on first use. Five lanes x three waves is
# ~70 min of pure rebuild, and the memory note `ccache-sizing-reuse-cliff` already
# records the cause: ~74% of ccache misses are first-compiles in fresh worktrees,
# and a 2-day-old entry almost never hits. Reusing the SAME worktrees across waves
# keeps each lane's build/ warm, so wave 2+ rebuilds only the delta.
#
# Pool worktrees live on the NVMe, in /mnt/fast/worktrees (CLAUDE.md), where
# lane_setup.sh gives each a real in-tree build/ and so cross-worktree ccache
# hits (#5010); see that script for why the build must be on the NVMe at all.
#
# Usage:
#   scripts/lane_pool.sh prepare [N] [BASE]   # create/refresh N lanes at BASE (default 5, origin/main)
#   scripts/lane_pool.sh reset   [N] [BASE]   # point existing lanes at a new BASE, keep build/
#   scripts/lane_pool.sh list
#   scripts/lane_pool.sh path <i>             # print lane i's path (for a lane brief)
#
# A lane agent is then told: "work in <path>, run `bash scripts/lane_setup.sh` first".
# NOTE: `reset` refuses a lane with uncommitted changes — a lane's WIP is never
# discarded silently (memory: agent-liveness-not-mtime, no-git-stash-in-worktrees).
set -uo pipefail

REPO="$(git rev-parse --show-toplevel)"
POOL_DIR="${BEZEL_LANE_POOL:-${BEZEL_WORKTREE_ROOT:-/mnt/fast/worktrees}}"
CMD="${1:-list}"; N="${2:-5}"; BASE="${3:-origin/main}"

# Named like the branch, since the directory is shared with every other worktree.
lane_path() { echo "$POOL_DIR/lane-pool-$1"; }

prepare() {
  mkdir -p "$POOL_DIR"
  git -C "$REPO" fetch origin --quiet 2>/dev/null || true
  for i in $(seq 1 "$N"); do
    p="$(lane_path "$i")"; b="lane-pool-$i"
    if [ -d "$p" ]; then echo "[lane_pool] lane-$i exists — use 'reset'"; continue; fi
    git -C "$REPO" branch -f "$b" "$BASE" >/dev/null 2>&1
    git -C "$REPO" worktree add --quiet "$p" "$b" || { echo "[lane_pool] add failed: $p"; continue; }
    ( cd "$p" && bash scripts/lane_setup.sh >/dev/null 2>&1 )
    echo "[lane_pool] prepared lane-$i -> $p (branch $b at $BASE)"
  done
  echo "[lane_pool] now warm each: cd <path> && scripts/agent_check.sh --probe /dev/null  (or any build)"
}

reset_lanes() {
  git -C "$REPO" fetch origin --quiet 2>/dev/null || true
  for i in $(seq 1 "$N"); do
    p="$(lane_path "$i")"; b="lane-pool-$i"
    [ -d "$p" ] || { echo "[lane_pool] lane-$i missing — run prepare"; continue; }
    if [ -n "$(git -C "$p" status --porcelain)" ]; then
      echo "[lane_pool] ⚠ lane-$i has UNCOMMITTED CHANGES — refusing to reset (inspect before discarding)"
      continue
    fi
    git -C "$p" checkout --quiet -B "$b" "$BASE" && \
      echo "[lane_pool] reset lane-$i to $BASE (build/ kept warm)"
  done
}

case "$CMD" in
  prepare) prepare ;;
  reset)   reset_lanes ;;
  path)    lane_path "${2:-1}" ;;
  list)    for i in $(seq 1 8); do p="$(lane_path "$i")"; [ -d "$p" ] && \
             printf "lane-%s  %s  %s  dirty=%s\n" "$i" "$p" \
               "$(git -C "$p" rev-parse --short HEAD 2>/dev/null)" \
               "$(git -C "$p" status --porcelain 2>/dev/null | wc -l)"; done ;;
  *)       sed -n '1,25p' "$0" ;;
esac
