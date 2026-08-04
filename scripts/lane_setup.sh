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

if [ ! -d "$FAST_ROOT" ]; then
  echo "[lane_setup] $FAST_ROOT missing (NVMe not mounted?) — leaving build/ where it is"
  exit 0
fi

TARGET="$FAST_ROOT/$NAME"

if [ -L "$WT/build" ]; then
  echo "[lane_setup] build/ already a symlink -> $(readlink "$WT/build")"
  exit 0
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
ln -sfn "$TARGET" "$WT/build"
echo "[lane_setup] build/ -> $TARGET  (NVMe; $(df -h "$FAST_ROOT" | awk 'NR==2{print $4}') free)"
