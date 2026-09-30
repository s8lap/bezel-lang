#!/usr/bin/env bash
# Consolidated garbage collector for worktrees, stale build dirs, and root
# scratch. SAFE BY DEFAULT: dry-run — it only LISTS what it would do. Pass
# --apply to act.
#
# Scopes (default: all): --worktrees --builds --root
#   --worktrees   reap worktrees whose commits are ALREADY in origin/main
#                 (branch ref is kept; only the checkout + inside build/ go).
#   --builds      remove stale /var/tmp/bezel-scratch/build-asan-* (keep the
#                 canonical one asan_sweep reuses for this repo).
#   --root        QUARANTINE (move, don't delete) stray root scratch older than
#                 BEZEL_GC_ROOT_AGE_HOURS (default 48h) into
#                 /var/tmp/bezel-scratch/root-quarantine/<ts>/ — reversible;
#                 the quarantine self-purges after 14 days.
#   --landed-only restrict worktree reaping to landed ones (used by the on-push
#                 hook in gate_and_push.sh); without it, --worktrees also reports
#                 (but never auto-reaps) clean-but-unmerged worktrees.
#
# NEVER touched: the current worktree, locked worktrees, worktrees with
# uncommitted TRACKED changes (WIP), and anything under the age threshold.
set -u
MAIN="$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gc: not in a git repo" >&2; exit 2; }
CUR="$MAIN"   # cwd worktree top; may differ from MAIN when run inside a worktree
MAIN="$(git -C "$MAIN" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | xargs dirname 2>/dev/null || echo "$MAIN")"

APPLY=0; DO_WT=0; DO_BUILDS=0; DO_ROOT=0; LANDED_ONLY=0
AGE_H=${BEZEL_GC_ROOT_AGE_HOURS:-48}
QUAR=/var/tmp/bezel-scratch/root-quarantine
for a in "$@"; do case "$a" in
  --apply) APPLY=1 ;;
  --worktrees) DO_WT=1 ;;
  --builds) DO_BUILDS=1 ;;
  --root) DO_ROOT=1 ;;
  --landed-only) LANDED_ONLY=1; DO_WT=1 ;;
  *) echo "gc: unknown arg '$a'" >&2; exit 2 ;;
esac; done
[ $((DO_WT+DO_BUILDS+DO_ROOT)) -eq 0 ] && { DO_WT=1; DO_BUILDS=1; DO_ROOT=1; }
tag() { [ "$APPLY" = 1 ] && printf '[gc] %s\n' "$*" || printf '[gc dry-run] would %s\n' "$*"; }

REPO_ROOT="$(git rev-parse --show-toplevel)"   # top of THIS invocation's tree

# ── worktrees ────────────────────────────────────────────────────────────
if [ "$DO_WT" = 1 ]; then
  git -C "$REPO_ROOT" fetch -q origin main 2>/dev/null || true  # best-effort freshen
  locked="$(git -C "$REPO_ROOT" worktree list --porcelain | awk '/^worktree /{p=$2} /^locked/{print p}')"
  # One pass over /proc for every live process's cwd. Used below to spot a
  # worktree some process is actually sitting in. Cheap; done once, not per-wt.
  live_cwds="$(readlink /proc/[0-9]*/cwd 2>/dev/null | sort -u)"
  git -C "$REPO_ROOT" worktree list --porcelain | awk '/^worktree /{print $2}' | while read -r wt; do
    [ "$wt" = "$MAIN" ] && continue     # never the canonical main worktree
    [ "$wt" = "$CUR" ] && continue      # nor the worktree we are running in
    printf '%s\n' "$locked" | grep -qxF "$wt" && continue
    if [ -n "$(git -C "$wt" status --porcelain -uno 2>/dev/null)" ]; then
      echo "[gc] KEEP (WIP, uncommitted): $wt"; continue
    fi
    # ── IN-USE guards ───────────────────────────────────────────────────────
    # A REUSED warm-pool worktree (/mnt/fast/worktrees/<name>) becomes "landed"
    # the instant its batch is pushed — which is exactly when the next lane is
    # starting work in it. Since gate_and_push.sh runs this script with --apply
    # after every successful push, the WIP + unmerged guards alone would delete
    # a live lane's tree mid-run (a 2026-07-30 dry-run offered to reap all four
    # pool worktrees while four lanes were working in them). A freshly-created
    # pool worktree at origin/main has NO commits and NO dirty files, so it
    # passes both existing guards; only these two catch it.
    #
    # (1) Some live process is cwd'd inside it. Uses cwd rather than cmdline
    #     matching, which is unreliable — a build under dev_heavy.sh does not
    #     carry the worktree path in its argv. String prefix, not regex, so
    #     paths containing regex metacharacters are safe.
    if printf '%s\n' "$live_cwds" |
         awk -v w="$wt" '$0==w || index($0, w"/")==1 {f=1} END{exit !f}'; then
      echo "[gc] KEEP (IN USE: a live process is cwd'd inside it): $wt"; continue
    fi
    # (2) Recently active. (1) alone is not sufficient: a lane sitting between
    #     tool calls has no live process in its tree at that instant. The git
    #     index is touched by checkout/commit/status, so its mtime tracks lane
    #     activity. Conservative by design — a finished worktree is merely
    #     reaped one cycle later. Override with BEZEL_GC_WT_IDLE_MIN=0.
    idle_min="${BEZEL_GC_WT_IDLE_MIN:-120}"
    if [ "$idle_min" != 0 ] &&
       [ -n "$(find "$(git -C "$wt" rev-parse --git-dir 2>/dev/null || echo /nonexistent)/index" \
                    -newermt "-${idle_min} minutes" 2>/dev/null)" ]; then
      echo "[gc] KEEP (active within ${idle_min}m): $wt"; continue
    fi
    br="$(git -C "$wt" symbolic-ref --short HEAD 2>/dev/null || echo DETACHED)"
    # Landed = no commits on this branch lack a cherry-equivalent in origin/main.
    # `git cherry` marks '+' for not-yet-in-main, '-' for already-there. If cherry
    # itself fails (origin/main unknown), treat as unmerged so we never reap
    # unverified. NB grep -c exits 1 on zero matches but still prints "0", so it
    # is captured directly (no `|| echo 1`, which would corrupt the count).
    if cherry_out="$(git -C "$wt" cherry origin/main HEAD 2>/dev/null)"; then
      unmerged="$(printf '%s' "$cherry_out" | grep -c '^+')"
    else
      unmerged=1
    fi
    if [ "${unmerged}" != 0 ]; then
      [ "$LANDED_ONLY" = 1 ] && continue
      echo "[gc] KEEP (unmerged: $unmerged commit(s) not in origin/main): $wt ($br)"
      continue
    fi
    tag "reap landed worktree $wt  [branch $br kept]"
    [ "$APPLY" = 1 ] && git -C "$REPO_ROOT" worktree remove --force "$wt"
  done
  [ "$APPLY" = 1 ] && git -C "$REPO_ROOT" worktree prune
fi

# ── stale ASan build trees (keep this repo's canonical one) ───────────────
if [ "$DO_BUILDS" = 1 ]; then
  # ---------------------------------------------------------------------------
  # fix #3005: REAP ORPHANED LANE BUILD TREES on the fast disk.
  #
  # scripts/lane_setup.sh symlinks a lane worktree's build/ out to
  # /mnt/fast/lane-builds/<name>. `git worktree remove` then deletes only the
  # SYMLINK and orphans the tree. Nothing reaped these: before this block,
  # `grep -c lane-builds scripts/gc_scratch.sh` was 0. Measured 2026-08-08:
  # 68 GB across 54 orphans in four days (~17 GB/day), cleared by hand, having
  # taken /mnt/fast to 92% full. CLAUDE.md meanwhile asserted the invariant that
  # says this cannot happen — which is why nobody was watching the directory.
  #
  # ⚠ THE GUARDS ARE THE LOAD-BEARING PART. This project's own history records
  # gc_scratch's liveness guard once being one commit away from reaping THREE
  # ACTIVE worktrees. A tree is reaped only when ALL of these hold:
  #   (1) its basename is no longer a registered git worktree, AND
  #   (2) no live process has its cwd inside the tree, AND
  #   (3) it is older than the age floor, AND
  #   (4) it looks like a build tree (CMakeCache.txt present).
  # (4) means an unrecognised directory is never deleted, whatever its name.
  #
  # ⚠ DO NOT substitute a shallow mtime probe for (2). Every build tree here is
  # **Unix Makefiles**, not Ninja, and writes objects at depth 4-5 — a
  # `-maxdepth 2` mtime check reports "idle" for a lane that is actively
  # compiling. That mistake was proposed and measured wrong.
  LB_ROOT="${BEZEL_LANE_BUILD_ROOT:-/mnt/fast/lane-builds}"
  LB_AGE_MIN="${BEZEL_LANE_BUILD_AGE_MIN:-1440}"   # 24h
  # The mountpoint check applies to the DEFAULT root only: it exists so an
  # unmounted NVMe cannot make us reap a shadow directory on the slow disk
  # (#3008). An explicitly-overridden root is the caller's business, and
  # requiring a mountpoint there would make this block untestable.
  LB_MOUNT_OK=1
  if [ -z "${BEZEL_LANE_BUILD_ROOT:-}" ]; then
    mountpoint -q "$(dirname "$LB_ROOT")" 2>/dev/null || LB_MOUNT_OK=0
  fi
  if [ -d "$LB_ROOT" ] && [ "$LB_MOUNT_OK" = 1 ]; then
    # fix #3213: lane_setup.sh now names the tree `<basename>-<8-hex path
    # digest>`, because two worktrees sharing a basename used to share ONE build
    # tree and clobber each other's binaries mid-run. Match BOTH spellings: the
    # digest form for anything created since, and the bare basename so trees
    # predating the change are still recognised as live rather than reaped out
    # from under a running lane. Computed in bash, not inside awk — the awk form
    # needs four levels of nested quoting to shell out for cksum, which is a
    # cost with no benefit here.
    live_wts=""
    while read -r _wtpath; do
      [ -n "$_wtpath" ] || continue
      _b="$(basename "$_wtpath")"
      _dg="$(printf '%s' "$_wtpath" | cksum | awk '{printf "%08x", $1}')"
      live_wts="$live_wts$_b
$_b-$_dg
"
    done <<EOF_WTS
$(git -C "$REPO_ROOT" worktree list --porcelain 2>/dev/null | awk '/^worktree /{print $2}')
EOF_WTS
    for d in "$LB_ROOT"/*; do
      [ -d "$d" ] || continue
      base="$(basename "$d")"
      # (1) still a registered worktree?
      if printf '%s\n' "$live_wts" | grep -qxF "$base"; then continue; fi
      # (1b) the backref lane_setup.sh writes, if present, is authoritative
      _owner="$(cat "$d/.lane-worktree" 2>/dev/null || true)"
      if [ -n "$_owner" ] && [ -d "$_owner" ]; then continue; fi
      # A backref naming a directory that NO LONGER EXISTS is positive evidence
      # of an orphan, not merely absence of evidence — so the 24h floor below,
      # which exists to protect a tree whose worktree is still being set up,
      # does not apply. Measured 2026-08-14: six such trees held 18.7 GB while
      # /mnt/fast sat at 95%, all under the floor and therefore un-reapable, and
      # had to be cleared by hand. A lane that runs out of disk mid-build fails
      # in ways that look like a compiler error, so this is a correctness lever
      # and not only a housekeeping one.
      _proven_orphan=0
      [ -n "$_owner" ] && [ ! -d "$_owner" ] && _proven_orphan=1
      # (2) any live process working inside it?
      if ls -l /proc/*/cwd 2>/dev/null | grep -qF -- "$d"; then
        tag "KEEP (live process cwd inside): $d"; continue
      fi
      # (3) age floor — shortened when (1b) PROVED the owner is gone
      _floor="$LB_AGE_MIN"
      [ "$_proven_orphan" = 1 ] && _floor="${BEZEL_LANE_BUILD_ORPHAN_AGE_MIN:-120}"
      if [ -n "$(find "$d" -maxdepth 0 -mmin "-$_floor" 2>/dev/null)" ]; then continue; fi
      # (4) shaped like a build tree?
      if [ ! -e "$d/CMakeCache.txt" ]; then
        tag "KEEP (not a configured build tree — refusing to guess): $d"; continue
      fi
      tag "reap orphaned lane build tree $d ($(du -Lsh "$d" 2>/dev/null | cut -f1))"   # symlink-read-ok
      [ "$APPLY" = 1 ] && rm -rf "$d"
    done
  fi

  canon="build-asan-$(printf '%s' "$MAIN" | cksum | cut -d' ' -f1)"
  for d in /var/tmp/bezel-scratch/build-asan*; do
    [ -d "$d" ] || continue
    [ "$(basename "$d")" = "$canon" ] && continue
    tag "remove stale ASan build $d ($(du -sh "$d" 2>/dev/null | cut -f1))"
    [ "$APPLY" = 1 ] && rm -rf "$d"
  done
  # AOT test link-cache (tests/support/aot_support.hpp). The harness self-GCs it
  # to ~2.5 GiB once per process when it exceeds ~5 GiB, but that only runs while
  # a test process is alive; reap the whole store here as a backstop when it has
  # grown past a generous cap (e.g. no test has run to trigger the in-process GC).
  # Dropping it merely forces a re-warm — the next run recompiles on a miss.
  binc=/var/tmp/bezel-scratch/aot-bincache
  if [ -d "$binc" ]; then
    # Runaway backstop ONLY, sized ABOVE the harness's own in-process GC cap
    # (BEZEL_AOT_CACHE_MAX_GB, default 64 GiB — aot_support.hpp maybeGcCache,
    # which trims oldest-first to 3/4 of cap). Under the k2 identity (#2100)
    # cache entries are DESIGNED to persist across gates; the previous 6 GiB
    # wholesale rm -rf here deleted the ~17-30 GiB working set after every
    # push, forcing the next gate cold. Fire only when the in-process GC has
    # evidently failed (dir far beyond its cap).
    # fix #3004: `du -sk` (no -L) on a SYMLINKED cache root returns 4 (the link
    # itself), so `4 -gt 75497472` was false forever and this backstop has been
    # dead code since the symlink was created on 2026-07-31. Measured 2026-08-08:
    #   du -sk  -> 4          du -Lsk -> 53,463,648
    # This is the THIRD instance of the class (#3000 gate.sh find, #3004 here);
    # audit any du/find against a cache root for -L.  # symlink-read-ok
    #
    # The `rm -rf "$binc"` was a second, worse bug hiding behind the first: on a
    # symlink that deletes the LINK, not the store. It would have freed 0 bytes,
    # orphaned 50+ GiB on the NVMe with nothing pointing at it, and silently
    # relocated the cache to sda3 — the slow disk this whole layout exists to
    # avoid. Resolve the link and clear its CONTENTS.
    szk="$(du -Lsk "$binc" 2>/dev/null | cut -f1)"   # symlink-read-ok
    if [ -n "$szk" ] && [ "$szk" -gt $((72*1024*1024)) ]; then
      _bt="$(readlink -f "$binc" 2>/dev/null || printf '%s' "$binc")"
      tag "trim runaway AOT link-cache $_bt ($(du -Lsh "$binc" 2>/dev/null | cut -f1) > 72G backstop)"
      if [ "$APPLY" = 1 ] && [ -d "$_bt" ]; then
        find "$_bt" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
      fi
    fi
  fi
fi

# ── root scratch → quarantine (age-gated, reversible) ─────────────────────
# Always targets the canonical MAIN worktree root (the shared root we keep
# clean), regardless of which worktree invoked the GC.
if [ "$DO_ROOT" = 1 ]; then
  ts="$(date +%Y%m%d-%H%M%S 2>/dev/null || echo run)"
  dest="$QUAR/$ts"
  git -C "$MAIN" status --porcelain --untracked-files=normal | grep '^??' | sed 's/^?? //' | while read -r e; do
    e="${e%/}"
    p="$MAIN/$e"
    # Ephemeral by NAME, or a stray ELF executable dropped by a probe compile.
    keep=1
    case "$e" in
      probe*|repro*|sw22_*|p[0-9]*|test_*|*.bz|*.bzl|scratch_*|tmp_*|out_*|hello_*|sanity*) keep=0 ;;
      *) if [ -f "$p" ] && [ "$(head -c4 "$p" 2>/dev/null | od -An -tx1 2>/dev/null | tr -d ' ')" = "7f454c46" ]; then keep=0; fi ;;
    esac
    [ "$keep" = 1 ] && continue
    # Age gate: only entries older than the threshold (protect active probes).
    [ -n "$(find "$p" -maxdepth 0 -mmin +$((AGE_H*60)) 2>/dev/null)" ] || continue
    tag "quarantine root scratch '$e' -> $dest/"
    if [ "$APPLY" = 1 ]; then mkdir -p "$dest"; mv "$p" "$dest/" 2>/dev/null; fi
  done
  # Self-purge quarantine entries older than 14 days.
  if [ -d "$QUAR" ]; then
    find "$QUAR" -maxdepth 1 -mindepth 1 -type d -mtime +14 2>/dev/null | while read -r old; do
      tag "purge old quarantine $old"
      [ "$APPLY" = 1 ] && rm -rf "$old"
    done
  fi
fi
