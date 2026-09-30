#!/usr/bin/env bash
# lane_setup.sh — first command a fix-lane runs inside its worktree.
#
# WHY: a build tree on /home (sda3) does ~5 durable file creates/s; the NVMe at
# /mnt/fast does ~846/s. A cold lane build is create-dominated, so the build tree
# must be on the NVMe (see memory: storage-layout-ssd).
#
# Lane worktrees live on the NVMe, at /mnt/fast/worktrees/<name> (CLAUDE.md).
# There build/ is a REAL in-tree dir, which is what turns on the cross-worktree
# ccache keys (#5010). A worktree still on /home gets the older layout, a
# ./build -> /mnt/fast/lane-builds/<worktree-name> symlink (per-worktree ccache
# keys, no cross-worktree hits), and a warning naming the right location.
# Idempotent: safe to re-run, refuses to clobber a real (non-symlink) build dir
# that already holds objects.
#
# Usage:  bash scripts/lane_setup.sh [--owner <name>]
#   --owner <name>  also bind this worktree to <name> and its current branch
#                   (scripts/wt_owner.sh claim), so the ownership hooks refuse
#                   a commit here from anyone who switches it to another branch.
#                   Without the flag nothing about ownership changes.
set -u

OWNER=""
if [ "${1:-}" = "--owner" ]; then
  OWNER="${2:-}"
  [ -n "$OWNER" ] || { echo "[lane_setup] --owner needs a name" >&2; exit 64; }
fi

FAST_ROOT="${BEZEL_LANE_BUILD_ROOT:-/mnt/fast/lane-builds}"
# Where lane worktrees belong: a sibling of FAST_ROOT on the same disk.
WT_HOME="${BEZEL_WORKTREE_ROOT:-$(dirname "$FAST_ROOT")/worktrees}"

# Resolve the worktree root from git, not $PWD, so it works from a subdir.
WT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "[lane_setup] not inside a git worktree — skipping (build stays local)"; exit 0; }
# Before every early `exit 0` below, so the claim happens however build/ is set up.
if [ -n "$OWNER" ]; then
  bash "$(dirname -- "${BASH_SOURCE[0]}")/wt_owner.sh" claim "$OWNER" \
    || echo "[lane_setup] ownership claim failed — continuing without it"
fi
# fix #3213: KEY THE BUILD DIR ON THE FULL PATH, NOT THE BASENAME.
#
# This was `NAME="$(basename "$WT")"`. Two worktrees whose directories share a
# basename — trivially easy, since throwaway review worktrees get names like
# `lane`, `wt`, `tmp` — then symlink build/ to the SAME /mnt/fast tree and
# silently share it. Measured 2026-08-14: two live worktrees both named `lane`
# shared one build tree, and one agent's `agent_check.sh` removed the other's
# test binaries mid-run, producing an `env: … No such file or directory`
# failure that looks exactly like a broken test. A shared build tree between
# two agents is a correctness hazard, not just a slowdown: each reconfigure
# and each stale-binary removal is invisible cross-talk.
#
# The basename stays as a readable prefix so `ls /mnt/fast/lane-builds` is
# still navigable; an 8-hex digest of the ABSOLUTE path makes it unique.
_wt_digest="$(printf '%s' "$WT" | cksum | awk '{printf "%08x", $1}')"
NAME="$(basename "$WT")-$_wt_digest"

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

# fix #3213 (migration): a worktree set up before the digest existed has a warm
# tree at the OLD basename-only path. Adopt it by rename rather than orphaning a
# multi-GB configured build tree and rebuilding from cold — but ONLY when its
# backref names THIS worktree. A basename-only directory whose backref names
# someone else (or that has no backref at all) is exactly the collision this fix
# is about, so it is left alone for the reaper instead of being claimed.
_legacy="$FAST_ROOT/$(basename "$WT")"
if [ "$_legacy" != "$TARGET" ] && [ -d "$_legacy" ] && [ ! -d "$TARGET" ]; then
  if [ -r "$_legacy/.lane-worktree" ] && \
     [ "$(cat "$_legacy/.lane-worktree" 2>/dev/null)" = "$WT" ]; then
    if mv "$_legacy" "$TARGET" 2>/dev/null; then
      echo "[lane_setup] adopted legacy build tree $_legacy -> $TARGET"
    fi
  else
    echo "[lane_setup] $_legacy exists but is not ours (no matching backref) — not adopting"
  fi
fi

# AOT bincache + workdir symlinks on /mnt/fast — same rationale as the
# build/ symlink below, applied to the AOT cache and workdir that
# tests/support/aot_support.hpp reads by default. Without this:
#   - `cacheDir()` (tests/support/aot_support.hpp:1014) defaults to
#     /var/tmp/bezel-scratch/aot-bincache which is sda3 btrfs (~5 durable
#     creates/s); each cold-test cache store pays 4 fsyncs ≈260ms
#   - `scratchBase()` (aot_support.hpp:153) defaults to /var/tmp/bezel-scratch
#     which is the same sda3 btrfs; cross-filesystem hardlinks to the bincache
#     fail (EXDEV) and the harness falls back to per-test copy_file
# Together: a cold gate times out the 1800s shard ceiling per-test
# (~5.7s/test vs ~0.05s/test warm) — measured 2026-08-22 Wave 4 attempt-1,
# with cache_n=14284 invalid entries from the wrong tree identity. See
# memory: storage-layout-ssd (154x fsync gap), run-2026-08-22-impl.md
# (Wave 4 environmental fix).
#
# Same idempotency pattern as the build/ symlink below: detect /mnt/fast
# as a mount point (not just a directory — fix #3008's lesson), create
# only if missing, refuse to clobber a real dir with cache contents.
# Runs BEFORE the build/ block so it executes even when build/ is already
# correctly configured (the build/ block early-exits and would otherwise
# skip this section).
AOT_FAST_ROOT="${BEZEL_AOT_FAST_ROOT:-}"
# If unset, derive from BEZEL_LANE_BUILD_ROOT — the same var the build/
# symlink uses — so build and bincache share a fast-disk location by
# default. Override either independently if they need to live apart.
# If BOTH are unset, AOT_FAST_ROOT stays empty and the bincache section
# becomes a no-op (operator must opt in via env or set
# BEZEL_AOT_CACHE_DIR / BEZEL_AOT_SCRATCH_DIR to override the harness
# defaults at the C side).
if [ -z "$AOT_FAST_ROOT" ]; then
  _build_fast_root="${BEZEL_LANE_BUILD_ROOT:-}"
  if [ -n "$_build_fast_root" ]; then
    # FAST_ROOT is the parent of `<lane-name>` under lane-builds; strip
    # the trailing component to get the disk root. e.g.
    #   /mnt/fast/lane-builds       → /mnt/fast
    #   /scratch/builds            → /scratch
    AOT_FAST_ROOT="$(dirname "$_build_fast_root")"
  fi
fi
if [ -n "$AOT_FAST_ROOT" ] && mountpoint -q "$AOT_FAST_ROOT" 2>/dev/null; then
  for sub in aot-bincache aot-harness; do
    _link="/var/tmp/bezel-scratch/$sub"
    _tgt="$AOT_FAST_ROOT/$sub"
    if [ -L "$_link" ] && [ "$(readlink "$_link")" = "$_tgt" ]; then
      : # already correct — no-op
    elif [ -L "$_link" ]; then
      echo "[lane_setup] $_link -> $(readlink "$_link") != expected $_tgt — leaving alone"
    elif [ -d "$_link" ] && [ -z "$(ls -A "$_link" 2>/dev/null)" ]; then
      rmdir "$_link" && ln -sfn "$_tgt" "$_link" && \
        echo "[lane_setup] $_link -> $_tgt  (was empty dir; replaced with symlink)"
    elif [ -d "$_link" ]; then
      echo "[lane_setup] $_link is a NON-EMPTY real dir — leaving alone (move contents to $_tgt to convert)"
    else
      mkdir -p "$_tgt" 2>/dev/null && ln -sfn "$_tgt" "$_link" && \
        echo "[lane_setup] $_link -> $_tgt  (FAST_ROOT=$AOT_FAST_ROOT)"
    fi
  done
elif [ -n "$AOT_FAST_ROOT" ]; then
  echo "[lane_setup] $AOT_FAST_ROOT is NOT a mount point — bincache stays on sda3."
  echo "[lane_setup] A cold gate will time out the 1800s shard ceiling per-test."
  echo "[lane_setup] Mount the fast disk, or set BEZEL_AOT_FAST_ROOT / BEZEL_AOT_CACHE_DIR / BEZEL_AOT_SCRATCH_DIR."
else
  echo "[lane_setup] AOT fast-disk location not configured (no BEZEL_AOT_FAST_ROOT or BEZEL_LANE_BUILD_ROOT set)."
  echo "[lane_setup] Bincache + workdir stay on the default sda3 btrfs — cold gates will time out per-test."
  echo "[lane_setup] To enable: set BEZEL_AOT_FAST_ROOT=/your/fast/disk or BEZEL_AOT_CACHE_DIR / BEZEL_AOT_SCRATCH_DIR."
fi

# "On the fast disk" is decided by device number, not by path prefix (the #3008
# lesson: inspect what the name resolves to).
_wt_dev="$(stat -L -c %d "$WT" 2>/dev/null)"
_fast_dev="$(stat -L -c %d "$FAST_ROOT" 2>/dev/null)"
ON_FAST=0
[ -n "$_wt_dev" ] && [ "$_wt_dev" = "$_fast_dev" ] && ON_FAST=1
slow_disk_warning() {
  echo "[lane_setup] ⚠ this worktree is on the slow disk ($WT)."
  echo "[lane_setup]   Lane worktrees belong on the NVMe: git worktree add $WT_HOME/<name> ... (CLAUDE.md)."
  echo "[lane_setup]   Here build/ is a symlink with per-worktree ccache keys: no cross-worktree hits (#5010)."
}

if [ -L "$WT/build" ]; then
  if [ "$ON_FAST" = 1 ]; then
    # A worktree MOVED onto the fast disk keeps the build/ symlink it had on
    # /home. The linked tree cannot serve it (CMakeCache.txt binds a build tree
    # to its source path, so the next configure fails), and the symlink turns
    # off the relocatable keys below. Replace the LINK only; the old tree's
    # backref names a path that is gone, so gc_scratch.sh reaps it.
    echo "[lane_setup] worktree is on the fast disk but build/ -> $(readlink "$WT/build");" \
         "replacing the link with a real in-tree build/ (the old tree is left for gc_scratch.sh)"
    rm -f "$WT/build"
  # fix #3005: REPAIR A DANGLING LINK — this early return used to refuse to fix
  # the exact state this script creates. When /mnt/fast/lane-builds/<name> is
  # reaped (or was cleared by hand, as 68 GB of orphans were on 2026-08-08) the
  # worktree keeps a symlink pointing at nothing. `[ -L ]` is true, so the lane
  # was told "already a symlink" and every later cmake/build failed on a path
  # that does not exist. Nine live worktrees were measured in exactly this state.
  elif [ ! -e "$WT/build" ]; then
    echo "[lane_setup] build/ symlink DANGLES -> $(readlink "$WT/build") (target reaped) — repairing"
    rm -f "$WT/build"
  else
    echo "[lane_setup] build/ already a symlink -> $(readlink "$WT/build")"
    slow_disk_warning
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

# #5010: A WORKTREE THAT ALREADY LIVES ON THE FAST DISK GETS A REAL build/.
#
# The symlink exists only to move the build tree OFF the slow disk. When the
# worktree itself is on the fast filesystem there is nothing to move, and the
# symlink is actively harmful: CMake turns on relocatable ccache keys
# (BEZEL_CCACHE_RELOCATABLE — base_dir + -ffile-prefix-map, CMakeLists.txt)
# only for a build dir physically inside the tree, because ccache's base_dir
# rewrites paths relative to the PHYSICAL cwd. Through the symlink the keys
# keep the lane name and a fresh worktree misses on every TU (#4991 measured
# 0 of 3 shareable TUs); in-tree, it hits on everything but the handful of TUs
# that embed a per-tree path. ON_FAST is computed above.
if [ "$ON_FAST" = 1 ]; then
  mkdir -p "$WT/build" || { echo "[lane_setup] could not create $WT/build"; exit 0; }
  echo "[lane_setup] worktree is on the fast disk — build/ is a REAL in-tree dir" \
       "(cross-worktree ccache hits, #5010)"
  exit 0
fi

slow_disk_warning
mkdir -p "$TARGET" || { echo "[lane_setup] could not create $TARGET"; exit 0; }
# fix #3005: leave a BACKREF so a future orphan reaper can identify the owning
# worktree directly instead of inferring it from the directory basename. Nothing
# reaps this directory today (gc_scratch.sh has zero references to it), which is
# how it grew ~17 GB/day for four days unnoticed.
printf '%s\n' "$WT" > "$TARGET/.lane-worktree" 2>/dev/null || true
ln -sfn "$TARGET" "$WT/build"
echo "[lane_setup] build/ -> $TARGET  (NVMe; $(df -h "$FAST_ROOT" | awk 'NR==2{print $4}') free)"
