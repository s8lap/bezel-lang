#!/usr/bin/env bash
# lane_setup.sh — first command a fix-lane runs inside its worktree.
#
# WHY: a lane worktree's build/ tree lands on /home (sda3), which does ~5 durable
# file creates/s; the NVMe at /mnt/fast does ~846/s. A cold lane build is
# create-dominated, so putting the build tree on the NVMe is the single biggest
# per-lane throughput lever we have (see memory: storage-layout-ssd).
#
# This symlinks ./build -> /mnt/fast/lane-builds/<worktree-name> BEFORE the first
# configure, so agent_check.sh / cmake see a normal ./build path and nothing else
# changes. Idempotent: safe to re-run, refuses to clobber a real (non-symlink)
# build dir that already holds objects.
#
# Usage:  bash scripts/lane_setup.sh
set -u

FAST_ROOT="${BEZEL_LANE_BUILD_ROOT:-/mnt/fast/lane-builds}"

# Resolve the worktree root from git, not $PWD, so it works from a subdir.
WT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "[lane_setup] not inside a git worktree — skipping (build stays local)"; exit 0; }
NAME="$(basename "$WT")"

# fix #3008: GUARD ON THE MOUNT, NOT ON THE DIRECTORY.
#
# This was `[ ! -d "$FAST_ROOT" ]`. If the NVMe is not mounted, /mnt/fast
# resolves to plain btrfs on sda3 (measured — /mnt is NOT a mountpoint and there
# is no tmpfs involved, contrary to an earlier note), and fstab carries `nofail`
# so nothing reports the absence. The FIRST run under that condition CREATES the
# directory on the slow disk, after which `[ -d ]` passes forever and every lane
# silently builds on sda3 — the exact disk this script exists to avoid — into a
# tree that is shadowed the moment the NVMe comes back and is invisible to every
# GC.
#
# A directory existing says nothing about which filesystem it is on. That is the
# same mistake as reading a symlink without -L (#3000/#3004): inspecting a name
# instead of the thing the name resolves to.
if ! mountpoint -q "$(dirname "$FAST_ROOT")" 2>/dev/null; then
  echo "[lane_setup] $(dirname "$FAST_ROOT") is NOT a mount point — the fast disk is not mounted."
  echo "[lane_setup] Leaving build/ local rather than silently building on the slow disk."
  exit 0
fi
if [ ! -d "$FAST_ROOT" ]; then
  mkdir -p "$FAST_ROOT" 2>/dev/null || {
    echo "[lane_setup] cannot create $FAST_ROOT — leaving build/ where it is"; exit 0; }
fi

TARGET="$FAST_ROOT/$NAME"

if [ -L "$WT/build" ]; then
  # fix #3005: REPAIR A DANGLING LINK — this early return used to refuse to fix
  # the exact state this script creates. When /mnt/fast/lane-builds/<name> is
  # reaped (or was cleared by hand, as 68 GB of orphans were on 2026-08-08) the
  # worktree keeps a symlink pointing at nothing. `[ -L ]` is true, so the lane
  # was told "already a symlink" and every later cmake/build failed on a path
  # that does not exist. Nine live worktrees were measured in exactly this state.
  if [ ! -e "$WT/build" ]; then
    echo "[lane_setup] build/ symlink DANGLES -> $(readlink "$WT/build") (target reaped) — repairing"
    rm -f "$WT/build"
  else
    echo "[lane_setup] build/ already a symlink -> $(readlink "$WT/build")"
    exit 0
  fi
fi

if [ -d "$WT/build" ]; then
  # A real directory. If it already holds a configured tree, leave it alone —
  # relocating mid-flight would throw away cached objects and confuse cmake.
  if [ -e "$WT/build/CMakeCache.txt" ]; then
    echo "[lane_setup] build/ is a REAL configured dir — leaving as is (already warm)"
    exit 0
  fi
  rmdir "$WT/build" 2>/dev/null || {
    echo "[lane_setup] build/ is a non-empty real dir and not configured; leaving as is"; exit 0; }
fi

mkdir -p "$TARGET" || { echo "[lane_setup] could not create $TARGET"; exit 0; }
# fix #3005: leave a BACKREF so a future orphan reaper can identify the owning
# worktree directly instead of inferring it from the directory basename. Nothing
# reaps this directory today (gc_scratch.sh has zero references to it), which is
# how it grew ~17 GB/day for four days unnoticed.
printf '%s\n' "$WT" > "$TARGET/.lane-worktree" 2>/dev/null || true
ln -sfn "$TARGET" "$WT/build"
echo "[lane_setup] build/ -> $TARGET  (NVMe; $(df -h "$FAST_ROOT" | awk 'NR==2{print $4}') free)"
