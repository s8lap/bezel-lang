#!/usr/bin/env bash
# Consolidated garbage collector for worktrees, stale build dirs, and root
# scratch. SAFE BY DEFAULT: dry-run — it only LISTS what it would do. Pass
# --apply to act. Every run ends with a `[gc] summary` line: counts, and the
# bytes reclaimed (du of what goes; with --apply also the df delta per disk).
#
# Scopes (default: all): --worktrees --builds --root
#   --worktrees   reap LANDED worktrees (the branch ref is kept; only the
#                 checkout and its in-tree build/ go). LANDED means every
#                 commit on it is already in origin/main (`git cherry`), or —
#                 with --staging-ref — in REF by `git patch-id --stable`.
#   --builds      remove stale /var/tmp/bezel-scratch/build-asan-* (keep the
#                 canonical one asan_sweep reuses for this repo), and orphaned
#                 /mnt/fast/lane-builds/* trees.
#   --root        QUARANTINE (move, don't delete) stray root scratch older than
#                 BEZEL_GC_ROOT_AGE_HOURS (default 48h) into
#                 /var/tmp/bezel-scratch/root-quarantine/<ts>/ — reversible;
#                 the quarantine self-purges after 14 days.
#   --landed-only restrict worktree reaping to landed ones (used by the on-push
#                 hook in gate_and_push.sh); without it, --worktrees also reports
#                 (but never auto-reaps) clean-but-unmerged worktrees. Under
#                 --landed-only, LANDED means origin/main only unless
#                 --staging-ref is passed EXPLICITLY ($BEZEL_STAGING_REF is not
#                 read), so the gate's on-push GC behaves exactly as before.
#
# Worktree options (they imply --worktrees):
#   --staging-ref REF  also count a worktree as LANDED when each of its commits
#                 that is neither reachable from REF nor in origin/main has the
#                 same `git patch-id --stable` as a commit in REF ^origin/main.
#                 A run banks onto a staging branch and nothing reaches
#                 origin/main until the end-of-run gate, so without this a
#                 mid-run dry-run reaps almost nothing (measured 2026-11-05: 1
#                 of 160 worktrees, 106 "unmerged"). Patch-id, never a tree
#                 diff: a tree diff cannot tell "picked" from "reverted on top".
#                 A merge commit or an empty commit has no patch-id, so it
#                 counts as NOT landed. Default: $BEZEL_STAGING_REF if set.
#   --idle-builds for an IDLE worktree that must be KEPT (WIP, untracked files,
#                 unmerged), delete only its build/ (a configured tree with a
#                 CMakeCache.txt; a symlinked build/ deletes its target). The
#                 branch, the commits and the WIP stay. A rebuild is cheap:
#                 an in-tree build/ on the fast disk uses relocatable ccache
#                 keys (lane_setup.sh, #5010). Refused when any file under
#                 build/ changed within the idle threshold.
#   --protect FILE  extra PROTECTED worktrees, one path per line (`#` comments
#                 and blank lines allowed). The coordinator passes its live
#                 lanes' worktrees. A missing FILE is an error, not "nothing
#                 protected".
#
# PROTECTED — never touched, not even build/: the main checkout, the worktree
# this runs in, locked worktrees, any worktree a live process has its cwd in
# (at its top or anywhere beneath it), and every --protect path (a protected
# path also covers worktrees nested beneath it).
# KEPT (reported; build/ only with --idle-builds): uncommitted TRACKED changes
# (WIP), untracked files, unmerged commits, and anything whose git index changed
# within BEZEL_GC_WT_IDLE_MIN minutes (default 120; 0 disables the check — an
# active worktree is never touched at all).
# A detached HEAD that no branch, tag or remote ref contains is backed by a new
# branch `keep/<basename>-<sha8>` BEFORE its worktree is removed (and the
# removal is skipped if that branch cannot be made): a reap never loses a commit.
set -u
MAIN="$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gc: not in a git repo" >&2; exit 2; }
CUR="$MAIN"   # cwd worktree top; may differ from MAIN when run inside a worktree
MAIN="$(git -C "$MAIN" rev-parse --path-format=absolute --git-common-dir 2>/dev/null | xargs dirname 2>/dev/null || echo "$MAIN")"

APPLY=0; DO_WT=0; DO_BUILDS=0; DO_ROOT=0; LANDED_ONLY=0; IDLE_BUILDS=0
STAGING_REF=""; STAGING_REF_SET=0; PROTECT_FILE=""
AGE_H=${BEZEL_GC_ROOT_AGE_HOURS:-48}
QUAR=/var/tmp/bezel-scratch/root-quarantine
while [ $# -gt 0 ]; do case "$1" in
  --apply) APPLY=1 ;;
  --worktrees) DO_WT=1 ;;
  --builds) DO_BUILDS=1 ;;
  --root) DO_ROOT=1 ;;
  --landed-only) LANDED_ONLY=1; DO_WT=1 ;;
  --idle-builds) IDLE_BUILDS=1; DO_WT=1 ;;
  --staging-ref) [ $# -ge 2 ] && [ -n "$2" ] || { echo "gc: --staging-ref needs a ref" >&2; exit 2; }
                 STAGING_REF="$2"; STAGING_REF_SET=1; DO_WT=1; shift ;;
  --staging-ref=*) STAGING_REF="${1#*=}"; STAGING_REF_SET=1; DO_WT=1
                 [ -n "$STAGING_REF" ] || { echo "gc: --staging-ref needs a ref" >&2; exit 2; } ;;
  --protect) [ $# -ge 2 ] && [ -n "$2" ] || { echo "gc: --protect needs a file" >&2; exit 2; }
             PROTECT_FILE="$2"; shift ;;
  --protect=*) PROTECT_FILE="${1#*=}" ;;
  -h|--help) sed -n '2,62p' "$0"; exit 0 ;;
  *) echo "gc: unknown arg '$1'" >&2; exit 2 ;;
esac; shift; done
[ $((DO_WT+DO_BUILDS+DO_ROOT)) -eq 0 ] && { DO_WT=1; DO_BUILDS=1; DO_ROOT=1; }
# $BEZEL_STAGING_REF is the default only OUTSIDE --landed-only (see the header).
if [ "$STAGING_REF_SET" = 0 ] && [ "$LANDED_ONLY" = 0 ]; then STAGING_REF="${BEZEL_STAGING_REF:-}"; fi
if [ -n "$PROTECT_FILE" ] && [ ! -r "$PROTECT_FILE" ]; then
  echo "gc: --protect file '$PROTECT_FILE' is not readable — refusing to run with a protected set I cannot read" >&2
  exit 2
fi
tag() { [ "$APPLY" = 1 ] && printf '[gc] %s\n' "$*" || printf '[gc dry-run] would %s\n' "$*"; }

REPO_ROOT="$(git rev-parse --show-toplevel)"   # top of THIS invocation's tree

# ── accounting for the final summary ──────────────────────────────────────
RECLAIM_K=0        # du of everything removed (or, in a dry-run, to be removed)
declare -A DF0=()  # mount -> used KiB before (filled for every disk we touch)
du_k() { du -sk -- "$1" 2>/dev/null | cut -f1; }
human_k() { awk -v k="${1:-0}" 'BEGIN{ s="KMGTP"; i=1; while (k>=1024 && i<5){k/=1024;i++}
                                        printf (i==1?"%d%s":"%.1f%s"), k, substr(s,i,1) }'; }
note_disk() { # remember the df "used" of the filesystem holding $1, once
  local m; m="$(df -P -- "$1" 2>/dev/null | awk 'NR==2{print $6}')"
  [ -n "$m" ] && [ -z "${DF0[$m]:-}" ] && DF0[$m]="$(df -Pk -- "$m" | awk 'NR==2{print $3}')"
}
# reclaim <path> — add its du to the total and set RC (human size). Called
# directly, NEVER inside $(...): a subshell would drop both updates.
RC=""
reclaim() { local k; k="$(du_k "$1")"; RECLAIM_K=$((RECLAIM_K + ${k:-0})); note_disk "$1"; RC="$(human_k "${k:-0}")"; }

canon() { realpath -m -- "$1" 2>/dev/null || printf '%s\n' "$1"; }
# in_tree <path> <root> — path is root or beneath it. String prefix, not regex,
# so a path containing regex metacharacters is safe.
in_tree() { [ "$1" = "$2" ] || [ "${1#"$2"/}" != "$1" ]; }

# ── worktrees ────────────────────────────────────────────────────────────
N_SEEN=0 N_REAP_MAIN=0 N_REAP_STG=0 N_BUILDONLY=0 N_KEEPBR=0 N_PRUNABLE=0
N_PROT=0 N_INUSE=0 N_ACTIVE=0 N_WIP=0 N_UNTR=0 N_UNM=0 N_NOBUILD=0
if [ "$DO_WT" = 1 ]; then
  git -C "$REPO_ROOT" fetch -q origin main 2>/dev/null || true  # best-effort freshen
  HAVE_MAIN=0
  git -C "$REPO_ROOT" rev-parse -q --verify 'origin/main^{commit}' >/dev/null 2>&1 && HAVE_MAIN=1
  # Patch-ids of every non-merge commit in REF that origin/main does not have:
  # computed ONCE, then each worktree's own commits are looked up in it.
  STAGING_PIDS=""
  if [ -n "$STAGING_REF" ]; then
    if ! git -C "$REPO_ROOT" rev-parse -q --verify "$STAGING_REF^{commit}" >/dev/null 2>&1; then
      echo "gc: --staging-ref '$STAGING_REF' does not name a commit" >&2; exit 2
    fi
    _rng=("$STAGING_REF"); [ "$HAVE_MAIN" = 1 ] && _rng+=("^origin/main")
    STAGING_PIDS="$(git -C "$REPO_ROOT" log -p --no-merges --no-color --no-ext-diff "${_rng[@]}" 2>/dev/null |
                    git -C "$REPO_ROOT" patch-id --stable | cut -d' ' -f1 | sort -u)"
    echo "[gc] staging ref $STAGING_REF: $(printf '%s' "$STAGING_PIDS" | grep -c .) patch-id(s) beyond origin/main"
  fi

  # The PROTECTED set (see the header), canonicalised once.
  P_MAIN="$(canon "$MAIN")"; P_CUR="$(canon "$CUR")"
  mapfile -t P_LOCKED < <(git -C "$REPO_ROOT" worktree list --porcelain |
                          awk '/^worktree /{p=substr($0,10)} /^locked/{print p}' |
                          while read -r l; do canon "$l"; done)
  P_FILE=()
  if [ -n "$PROTECT_FILE" ]; then
    while IFS= read -r l || [ -n "$l" ]; do
      l="${l%%#*}"; l="$(printf '%s' "$l" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
      [ -n "$l" ] && P_FILE+=("$(canon "$l")")
    done < "$PROTECT_FILE"
    echo "[gc] --protect $PROTECT_FILE: ${#P_FILE[@]} path(s)"
  fi
  # One pass over /proc for every live process's cwd. Used below to spot a
  # worktree some process is actually sitting in. Cheap; done once, not per-wt.
  live_cwds="$(readlink /proc/[0-9]*/cwd 2>/dev/null | sort -u)"
  idle_min="${BEZEL_GC_WT_IDLE_MIN:-120}"

  # landed_in <wt> — prints `main`, `staging` or nothing (= not landed).
  # main: `git cherry` marks '+' for not-yet-in-main, '-' for already-there.
  # If cherry itself fails (origin/main unknown), treat as unmerged so we never
  # reap unverified. NB grep -c exits 1 on zero matches but still prints "0",
  # so it is captured directly (no `|| echo 1`, which would corrupt the count).
  landed_in() {
    local wt="$1" out plus n c pid
    if out="$(git -C "$wt" cherry origin/main HEAD 2>/dev/null)"; then
      [ "$(printf '%s' "$out" | grep -c '^+')" = 0 ] && { echo main; return; }
    fi
    [ -n "$STAGING_REF" ] || return 0
    # staging: the commits on this worktree that are in neither REF nor
    # origin/main by reachability must each match a REF patch-id. A merge has
    # no patch-id and an empty commit prints none: both count as not landed.
    local excl=("^$STAGING_REF"); [ "$HAVE_MAIN" = 1 ] && excl+=("^origin/main")
    [ "$(git -C "$wt" rev-list --merges --count HEAD "${excl[@]}" 2>/dev/null || echo 1)" = 0 ] || return 0
    local shas; shas="$(git -C "$wt" rev-list --no-merges HEAD "${excl[@]}" 2>/dev/null)" || return 0
    plus="$(printf '%s\n' "$out" | sed -n 's/^+ //p')"   # empty when cherry failed
    declare -A PID_OF=()
    while read -r pid c; do [ -n "$c" ] && PID_OF[$c]="$pid"; done < <(
      [ -n "$shas" ] && git -C "$wt" log -p --no-merges --no-color --no-ext-diff --no-walk=unsorted $shas 2>/dev/null |
        git -C "$wt" patch-id --stable)
    n=0
    for c in $shas; do
      # in origin/main by patch-id already (cherry printed it with '-')
      if [ "$HAVE_MAIN" = 1 ] && [ -n "$out" ] && ! printf '%s\n' "$plus" | grep -qxF "$c"; then continue; fi
      pid="${PID_OF[$c]:-}"
      if [ -n "$pid" ] && printf '%s\n' "$STAGING_PIDS" | grep -qxF "$pid"; then continue; fi
      n=$((n+1))
    done
    [ "$n" = 0 ] && echo staging
    return 0
  }
  # build_target <wt> — the build tree --idle-builds may delete, or nothing.
  # Only a CONFIGURED tree (CMakeCache.txt) is recognised; an unrecognised
  # directory is never deleted, whatever its name. A symlinked build/ resolves
  # to its target, which must not carry another worktree's backref.
  build_target() {
    local b="$1/build" t o
    if [ -L "$b" ]; then
      t="$(readlink -f -- "$b" 2>/dev/null)" || return 0
      [ -d "$t" ] && [ -e "$t/CMakeCache.txt" ] || return 0
      o="$(cat "$t/.lane-worktree" 2>/dev/null || true)"
      if [ -n "$o" ] && [ "$(canon "$o")" != "$(canon "$1")" ]; then return 0; fi
      printf '%s\n' "$t"
    elif [ -d "$b" ] && [ -e "$b/CMakeCache.txt" ]; then
      printf '%s\n' "$b"
    fi
  }
  # drop_build <wt> <why> — --idle-builds: delete only build/, keep everything else.
  drop_build() {
    local wt="$1" why="$2" t
    t="$(build_target "$wt")"
    if [ -z "$t" ]; then N_NOBUILD=$((N_NOBUILD+1)); return 0; fi
    if [ "$idle_min" != 0 ] && [ -n "$(find "$t" -newermt "-${idle_min} minutes" -print -quit 2>/dev/null)" ]; then
      echo "[gc] KEEP build/ (written within ${idle_min}m): $wt"; return 0
    fi
    N_BUILDONLY=$((N_BUILDONLY+1))
    reclaim "$t"
    tag "delete idle build/ of KEPT worktree $wt ($why) -> $t ($RC) [branch + WIP kept]"
    if [ "$APPLY" = 1 ]; then
      rm -rf -- "$t"
      [ -L "$wt/build" ] && rm -f -- "$wt/build"
    fi
  }
  # backstop <wt> <br> — a detached HEAD that no ref contains gets a keep/ branch
  # before its worktree goes. Returns non-zero if it could not make one.
  backstop() {
    local wt="$1" br="$2" h kb
    [ "$br" = DETACHED ] || return 0
    h="$(git -C "$wt" rev-parse -q --verify HEAD 2>/dev/null)" || return 1
    [ -n "$(git -C "$REPO_ROOT" for-each-ref --contains "$h" --count=1 refs/heads refs/remotes refs/tags 2>/dev/null)" ] && return 0
    kb="keep/$(basename "$wt")-${h:0:8}"
    N_KEEPBR=$((N_KEEPBR+1))
    tag "back detached HEAD ${h:0:12} of $wt with branch $kb"
    [ "$APPLY" = 1 ] || return 0
    if git -C "$REPO_ROOT" rev-parse -q --verify "refs/heads/$kb" >/dev/null 2>&1; then
      [ "$(git -C "$REPO_ROOT" rev-parse "refs/heads/$kb")" = "$h" ] && return 0
      echo "[gc] KEEP ($kb exists and points elsewhere — refusing to remove an unbacked HEAD): $wt"; return 1
    fi
    git -C "$REPO_ROOT" branch "$kb" "$h" || { echo "[gc] KEEP (could not create $kb): $wt"; return 1; }
  }
  # keep_wt <wt> <why> — a KEPT worktree; --idle-builds may drop its build/.
  keep_wt() {
    local wt="$1" why="$2"
    [ "$IDLE_BUILDS" = 1 ] && drop_build "$wt" "$why"
    return 0
  }

  while read -r wt; do
    [ -n "$wt" ] || continue
    N_SEEN=$((N_SEEN+1))
    cw="$(canon "$wt")"
    # ── PROTECTED (never touched, build/ included) ───────────────────────────
    if [ "$cw" = "$P_MAIN" ] || [ "$cw" = "$P_CUR" ]; then N_PROT=$((N_PROT+1)); continue; fi
    _l=0; for p in "${P_LOCKED[@]}"; do [ "$cw" = "$p" ] && { _l=1; break; }; done
    [ "$_l" = 1 ] && { N_PROT=$((N_PROT+1)); continue; }
    if [ ! -d "$wt" ]; then
      N_PRUNABLE=$((N_PRUNABLE+1)); echo "[gc] PRUNABLE (directory gone; \`git worktree prune\` drops it): $wt"; continue
    fi
    _p=""; for p in "${P_FILE[@]}"; do
      if in_tree "$cw" "$p" || in_tree "$p" "$cw"; then _p="$p"; break; fi
    done
    if [ -n "$_p" ]; then
      N_PROT=$((N_PROT+1)); echo "[gc] KEEP (PROTECTED by --protect: $_p): $wt"; continue
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
    # (1) Some live process is cwd'd inside it (its top or any directory
    #     beneath it). Uses cwd rather than cmdline matching, which is
    #     unreliable — a build under dev_heavy.sh does not carry the worktree
    #     path in its argv. String prefix, not regex, so paths containing regex
    #     metacharacters are safe.
    if printf '%s\n' "$live_cwds" |
         awk -v w="$cw" -v w2="$wt" '$0==w || index($0, w"/")==1 || $0==w2 || index($0, w2"/")==1 {f=1} END{exit !f}'; then
      N_INUSE=$((N_INUSE+1)); echo "[gc] KEEP (IN USE: a live process is cwd'd inside it): $wt"; continue
    fi
    # (2) Recently active. (1) alone is not sufficient: a lane sitting between
    #     tool calls has no live process in its tree at that instant. The git
    #     index is touched by checkout/commit/status, so its mtime tracks lane
    #     activity. Conservative by design — a finished worktree is merely
    #     reaped one cycle later. Override with BEZEL_GC_WT_IDLE_MIN=0. An
    #     ACTIVE worktree is not touched at all — --idle-builds included.
    if [ "$idle_min" != 0 ] &&
       [ -n "$(find "$(git -C "$wt" rev-parse --git-dir 2>/dev/null || echo /nonexistent)/index" \
                    -newermt "-${idle_min} minutes" 2>/dev/null)" ]; then
      N_ACTIVE=$((N_ACTIVE+1)); echo "[gc] KEEP (active within ${idle_min}m): $wt"; continue
    fi
    br="$(git -C "$wt" symbolic-ref --short HEAD 2>/dev/null || echo DETACHED)"
    # ── KEPT: WIP, untracked files, unmerged ────────────────────────────────
    if [ -n "$(git -C "$wt" status --porcelain -uno 2>/dev/null)" ]; then
      N_WIP=$((N_WIP+1)); echo "[gc] KEEP (WIP, uncommitted): $wt"; keep_wt "$wt" WIP; continue
    fi
    # Untracked (not ignored) files are WIP too — a new test a lane has not
    # `git add`ed yet. `worktree remove --force` would delete them.
    if git -C "$wt" status --porcelain 2>/dev/null | grep -q '^??'; then
      N_UNTR=$((N_UNTR+1)); echo "[gc] KEEP (untracked files): $wt"; keep_wt "$wt" untracked; continue
    fi
    where="$(landed_in "$wt")"
    if [ -z "$where" ]; then
      N_UNM=$((N_UNM+1))
      if [ "$LANDED_ONLY" = 0 ]; then
        echo "[gc] KEEP (unmerged: commit(s) in neither origin/main${STAGING_REF:+ nor $STAGING_REF}): $wt ($br)"
      fi
      keep_wt "$wt" unmerged; continue
    fi
    backstop "$wt" "$br" || continue
    if [ "$where" = main ]; then N_REAP_MAIN=$((N_REAP_MAIN+1)); else N_REAP_STG=$((N_REAP_STG+1)); fi
    if [ "$where" = main ]; then _in="origin/main"; else _in="$STAGING_REF by patch-id"; fi
    reclaim "$wt"
    if [ "$br" = DETACHED ]; then _br="detached HEAD; a ref keeps its commit"; else _br="branch $br kept"; fi
    tag "reap landed worktree $wt  [$_br; landed in $_in] ($RC)"
    [ "$APPLY" = 1 ] && git -C "$REPO_ROOT" worktree remove --force "$wt"
  done < <(git -C "$REPO_ROOT" worktree list --porcelain | awk '/^worktree /{print substr($0,10)}')
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
      reclaim "$d"
      tag "reap orphaned lane build tree $d ($RC)"
      [ "$APPLY" = 1 ] && rm -rf "$d"
    done
  fi

  canon="build-asan-$(printf '%s' "$MAIN" | cksum | cut -d' ' -f1)"
  for d in /var/tmp/bezel-scratch/build-asan*; do
    [ -d "$d" ] || continue
    [ "$(basename "$d")" = "$canon" ] && continue
    reclaim "$d"
    tag "remove stale ASan build $d ($RC)"
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

# ── summary ───────────────────────────────────────────────────────────────
# One line a heartbeat or a log reader can grep. In a dry-run the bytes are
# what WOULD go (du); with --apply the df delta per touched disk follows, which
# is the honest number (du double-counts nothing here, but a removal can fail).
_mode=dry-run; [ "$APPLY" = 1 ] && _mode=apply
if [ "$DO_WT" = 1 ]; then
  echo "[gc] summary ($_mode): worktrees seen=$N_SEEN reaped=$((N_REAP_MAIN+N_REAP_STG)) (landed-main=$N_REAP_MAIN landed-staging=$N_REAP_STG) build-only=$N_BUILDONLY keep-branches=$N_KEEPBR kept: protected=$N_PROT in-use=$N_INUSE active=$N_ACTIVE wip=$N_WIP untracked=$N_UNTR unmerged=$N_UNM prunable=$N_PRUNABLE"
fi
echo "[gc] summary ($_mode): reclaim $( [ "$APPLY" = 1 ] && echo "" || echo "would be " )$(human_k "$RECLAIM_K") (du of what $( [ "$APPLY" = 1 ] && echo went || echo would go ))"
if [ "$APPLY" = 1 ]; then
  for m in "${!DF0[@]}"; do
    _u1="$(df -Pk -- "$m" | awk 'NR==2{print $3}')"
    echo "[gc] summary (apply): $m used $(human_k "${DF0[$m]}") -> $(human_k "$_u1") (freed $(human_k $(( ${DF0[$m]} - _u1 > 0 ? ${DF0[$m]} - _u1 : 0 ))))"
  done
fi
