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
  git -C "$REPO_ROOT" worktree list --porcelain | awk '/^worktree /{print $2}' | while read -r wt; do
    [ "$wt" = "$MAIN" ] && continue     # never the canonical main worktree
    [ "$wt" = "$CUR" ] && continue      # nor the worktree we are running in
    printf '%s\n' "$locked" | grep -qxF "$wt" && continue
    if [ -n "$(git -C "$wt" status --porcelain -uno 2>/dev/null)" ]; then
      echo "[gc] KEEP (WIP, uncommitted): $wt"; continue
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
    szk="$(du -sk "$binc" 2>/dev/null | cut -f1)"
    if [ -n "$szk" ] && [ "$szk" -gt $((72*1024*1024)) ]; then
      tag "trim runaway AOT link-cache $binc ($(du -sh "$binc" 2>/dev/null | cut -f1) > 72G backstop)"
      [ "$APPLY" = 1 ] && rm -rf "$binc"
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
