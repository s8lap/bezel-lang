#!/usr/bin/env bash
#
# shared_build.sh — build bezelc + the test binaries for a commit ONCE, publish
# the result read-only, and let read-only lanes (reviewers, issue-verifiers)
# run against it instead of linking and storing their own ~3.6 GB build.
#
# WHY. ccache already shares the COMPILES between worktrees (#5010), but every
# lane still links and stores its own bezelc plus six test binaries, and a
# reviewer builds TWO arms (base + tip). A read-only lane at a commit someone
# has already built only needs to RUN binaries, never to build them.
#
# LAYOUT.  $BEZEL_SHARED_BUILD_ROOT (default /mnt/fast/shared-builds):
#   <full-sha>/MANIFEST           written LAST (tmp + rename): the publish marker
#   <full-sha>/tree/              a detached, LOCKED git worktree at <sha>
#   <full-sha>/tree/build/        its build dir (bezelc, tests/bezel_tests_*)
#   <full-sha>/bezelc             -> tree/build/bezelc
#   <full-sha>/test_binaries.txt  copy of tree/build/tests/test_binaries.txt
#   <full-sha>/publish.log        configure + build output
#   .locks/<sha>.build            flock: one builder per sha (a 2nd one waits)
#   .locks/<sha>.use              flock -s held by every reader; prune skips it
#
# WHY THE BUILD HAPPENS IN PLACE, NOT IN A TEMP DIR THAT IS THEN RENAMED.
# The test binaries carry five ABSOLUTE per-build paths (tests/CMakeLists.txt,
# the generated build_paths.cpp: libbezel_runtime.a, libbezel_core.a, bezelc,
# <repo>/src/runtime, <repo>), and Driver.cpp carries the runtime archive's
# absolute path too. None has a run-time override. A binary copied or renamed
# elsewhere therefore keeps linking against, exec'ing, and reading sources
# from the tree that BUILT it — which, for a single reused build worktree,
# would be whatever sha was built there LAST. So each sha is built in its own
# worktree at its FINAL path, and atomicity comes from the marker instead:
# a directory without MANIFEST is never served, and MANIFEST is renamed into
# place only after every artefact is verified. The verify step also checks that
# each test binary really embeds THIS build's bezelc path.
#
# KEY. The MANIFEST records `treekey` = sha1 over `git rev-parse <sha>:<p>` for
# p in src tests CMakeLists.txt cmake (a missing path counts as "-"). Two
# commits with the same key build the same binaries (bezelc --version differs:
# it embeds `git describe`), so a lane whose CLEAN tree has a published key
# can reuse that build (`resolve auto`, `agent_check.sh --use-shared auto`).
# The key does not cover the system toolchain (clang/LLVM); the MANIFEST
# records `toolchain` so a mismatch can be seen.
#
# Usage:
#   shared_build.sh publish [--ref REF] [--keep N]  build+publish REF (default
#                                 $BEZEL_STAGING_REF, else r30-c1), then prune
#   shared_build.sh ensure REF    publish REF unless already published (flocked:
#                                 a concurrent ensure of the same sha WAITS)
#   shared_build.sh path REF      print the published dir for REF's exact sha;
#                                 exit 1 if none (never builds)
#   shared_build.sh path --tree REF   same, but any published sha whose treekey
#                                 equals REF's qualifies (exact sha preferred)
#   shared_build.sh treekey REF   print REF's treekey
#   shared_build.sh check-clean [--worktree DIR] REF
#                                 exit 3 (with the reason) if DIR's src/ tests/
#                                 CMakeLists.txt cmake differ from REF's,
#                                 including untracked files there
#   shared_build.sh resolve [--worktree DIR] REF|auto
#                                 check-clean + lookup, for agent_check.sh:
#                                 prints dir= build= lock= sha= match= lines.
#                                 auto = DIR's HEAD. exit 1 none, 3 dirty.
#   shared_build.sh offer [--worktree DIR]   one stderr NOTE if DIR is clean
#                                 and its HEAD's treekey is published; always 0
#   shared_build.sh list          published builds, newest first
#   shared_build.sh prune [--keep N]   keep the newest N (default 4); never
#                                 removes a build a reader holds or one being built
#
# Env: BEZEL_SHARED_BUILD_ROOT, BEZEL_SHARED_BUILD_KEEP (4), JOBS (4),
#      BEZEL_SHARED_BUILD_MIN_TOKENS (6: wait for that many free jobserver
#      tokens before building, when the jobserver runs; 0 disables),
#      BEZEL_SHARED_BUILD_TEST_BUILDER (TEST-ONLY: a command run instead of the
#      real cmake configure+build, with SB_TREE/SB_BUILD/SB_SHA in its env).
#
# The real build goes through scripts/dev_heavy.sh, so the shared slice and the
# machine-wide jobserver apply exactly as they do to a lane build.

set -uo pipefail

SELF_DIR="$(cd "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "shared_build: not inside a git worktree" >&2; exit 2; }
SHARED_ROOT="${BEZEL_SHARED_BUILD_ROOT:-/mnt/fast/shared-builds}"
LOCKS="$SHARED_ROOT/.locks"
KEEP="${BEZEL_SHARED_BUILD_KEEP:-4}"
JOBS="${JOBS:-4}"
MIN_TOKENS="${BEZEL_SHARED_BUILD_MIN_TOKENS:-6}"
DEFAULT_REF="${BEZEL_STAGING_REF:-r30-c1}"
KEY_PATHS=(src tests CMakeLists.txt cmake)

die() { echo "shared_build: $*" >&2; exit 2; }
say() { echo "shared_build: $*" >&2; }

# The root holds builds and nothing else we may delete: an empty value or "/"
# is refused outright (prune/publish rm -rf below it), and every removal below
# touches only names that are exactly a full lowercase git sha.
case "${BEZEL_SHARED_BUILD_ROOT-x}" in
  "") die "BEZEL_SHARED_BUILD_ROOT is set but empty — refusing" ;;
esac
case "${SHARED_ROOT%/}" in
  ""|/) die "BEZEL_SHARED_BUILD_ROOT must not be / — refusing" ;;
esac
SHARED_ROOT="${SHARED_ROOT%/}"
LOCKS="$SHARED_ROOT/.locks"
is_sha() { [[ "${1:-}" =~ ^[0-9a-f]{40}$ ]]; }

resolve_sha() {  # $1 ref [$2 repo dir] -> full sha, or rc 1
  git -C "${2:-$ROOT}" rev-parse --verify --quiet "$1^{commit}" 2>/dev/null
}
need_sha() {
  local s; s="$(resolve_sha "$1" "${2:-$ROOT}")" || die "cannot resolve '$1' to a commit"
  printf '%s\n' "$s"
}

tree_components() {  # $1 sha -> "p=<tree-or-blob>|-" lines
  local p h
  for p in "${KEY_PATHS[@]}"; do
    h="$(git -C "$ROOT" rev-parse --verify --quiet "$1:$p" 2>/dev/null)" || h="-"
    printf '%s=%s\n' "$p" "$h"
  done
}
tree_key() { tree_components "$1" | sha1sum | cut -d' ' -f1; }

mf_get() {  # $1 manifest $2 key
  awk -v k="$2" 'index($0, k "=") == 1 { print substr($0, length(k) + 2); exit }' "$1" 2>/dev/null
}
# A dir is SERVED only if its MANIFEST exists and names that very sha.
complete_dir() {  # $1 sha -> rc
  local d="$SHARED_ROOT/$1"
  [ -r "$d/MANIFEST" ] && [ "$(mf_get "$d/MANIFEST" sha)" = "$1" ]
}
published_shas() {  # newest first (nanosecond publish time), complete only
  local m s e
  for m in "$SHARED_ROOT"/*/MANIFEST; do
    [ -r "$m" ] || continue
    s="$(mf_get "$m" sha)"; e="$(mf_get "$m" built_at_ns)"
    is_sha "$s" && [ "$(basename "$(dirname "$m")")" = "$s" ] || continue
    printf '%s %s\n' "${e:-0}" "$s"
  done | sort -k1,1nr | cut -d' ' -f2
}
find_by_key() {  # $1 sha (preferred) $2 key -> sha of a published build, or rc 1
  complete_dir "$1" && { printf '%s\n' "$1"; return 0; }
  local s
  for s in $(published_shas); do
    [ "$(mf_get "$SHARED_ROOT/$s/MANIFEST" treekey)" = "$2" ] && { printf '%s\n' "$s"; return 0; }
  done
  return 1
}

warn_toolchain() {  # $1 sha: loud stderr line if the build's toolchain differs
  local built now
  built="$(mf_get "$SHARED_ROOT/$1/MANIFEST" toolchain)"
  now="$(clang++ --version 2>/dev/null | head -1)"
  [ -n "$built" ] && [ -n "$now" ] && [ "$built" != "$now" ] || return 0
  say "WARNING: toolchain mismatch for shared build ${1:0:12}: built with '$built', current is '$now'."
  say "  The treekey does not cover the toolchain; results may differ from a local build."
}

# --- check-clean: the published binaries were built from <ref>'s sources ------
check_clean() {  # $1 worktree $2 sha $3 label -> rc 0 clean, 3 dirty (reason on stderr)
  local wt="$1" sha="$2" label="$3" changed untracked
  changed="$(git -C "$wt" diff --name-only "$sha" -- "${KEY_PATHS[@]}" 2>/dev/null)" || {
    say "REFUSED: 'git diff' failed in $wt — cannot prove it matches $label (${sha:0:12})."; return 3; }
  untracked="$(git -C "$wt" ls-files --others --exclude-standard -- "${KEY_PATHS[@]}" 2>/dev/null)" || {
    say "REFUSED: 'git ls-files' failed in $wt — cannot prove it matches $label (${sha:0:12})."; return 3; }
  if [ -z "$changed" ] && [ -z "$untracked" ]; then return 0; fi
  say "REFUSED: $wt differs from $label (${sha:0:12}) in ${KEY_PATHS[*]}."
  say "  The shared binaries were built from $label's sources, so running them here"
  say "  would test code that is not the code in this worktree (stale binaries)."
  [ -z "$changed" ] || { say "  changed vs ${sha:0:12}:"; printf '%s\n' "$changed" | head -10 | sed 's/^/    /' >&2; }
  [ -z "$untracked" ] || { say "  untracked:"; printf '%s\n' "$untracked" | head -10 | sed 's/^/    /' >&2; }
  say "  Build normally (drop --use-shared), or commit/clean and use a ref whose tree matches."
  return 3
}

# --- removal (prune / an abandoned partial build) -----------------------------
remove_build_dir() {  # $1 dir
  local d="$1"
  chmod -R u+w "$d" 2>/dev/null || true
  if [ -e "$d/tree/.git" ]; then
    git -C "$ROOT" worktree unlock "$d/tree" >/dev/null 2>&1 || true
    git -C "$ROOT" worktree remove --force --force "$d/tree" >/dev/null 2>&1 || true
  fi
  rm -rf "$d"
  git -C "$ROOT" worktree prune >/dev/null 2>&1 || true
}

cmd_prune() {
  local keep="$KEEP"
  while [ $# -gt 0 ]; do
    case "$1" in --keep) keep="${2:-}"; shift 2 ;; *) die "prune: unknown arg '$1'" ;; esac
  done
  case "$keep" in ''|*[!0-9]*) die "--keep must be a non-negative integer" ;; esac
  [ -d "$SHARED_ROOT" ] || die "prune: shared root '$SHARED_ROOT' does not exist — refusing"
  mkdir -p "$LOCKS" || die "cannot create $LOCKS"
  local pfd ufd bfd s d n=0
  exec {pfd}>"$LOCKS/prune"; flock "$pfd"
  for s in $(published_shas); do
    n=$((n + 1)); [ "$n" -le "$keep" ] && continue
    exec {ufd}>"$LOCKS/$s.use"
    if ! flock -n -x "$ufd"; then say "prune: ${s:0:12} is in use by a reader — kept"; exec {ufd}>&-; continue; fi
    exec {bfd}>"$LOCKS/$s.build"
    if flock -n "$bfd"; then
      say "prune: removing ${s:0:12}"
      remove_build_dir "$SHARED_ROOT/$s"
      # Lock files are never unlinked: a waiter blocked on the old inode and a
      # later locker on a new one would both hold "the" lock.
    fi
    exec {bfd}>&- {ufd}>&-
  done
  # A dir with no MANIFEST whose build lock is free is an abandoned build.
  for d in "$SHARED_ROOT"/*/; do
    d="${d%/}"; s="$(basename "$d")"
    is_sha "$s" && [ -d "$d" ] && ! complete_dir "$s" || continue
    exec {bfd}>"$LOCKS/$s.build"
    if flock -n "$bfd"; then say "prune: removing abandoned partial build ${s:0:12}"; remove_build_dir "$d"; fi
    exec {bfd}>&-
  done
  exec {pfd}>&-
  return 0
}

# --- the build ----------------------------------------------------------------
wait_for_tokens() {
  [ "$MIN_TOKENS" -gt 0 ] 2>/dev/null || return 0
  local js="$SELF_DIR/build_jobserver.py" st av waited=0
  [ -r "$js" ] || return 0
  while :; do
    st="$(python3 "$js" status 2>/dev/null)" || return 0   # no jobserver: nothing to wait for
    av="$(printf '%s\n' "$st" | sed -n 's/.*available=\([0-9]*\).*/\1/p' | head -1)"
    [ -n "$av" ] || return 0
    [ "$av" -ge "$MIN_TOKENS" ] && return 0
    [ $((waited % 300)) -eq 0 ] && say "build gate: jobserver available=$av < $MIN_TOKENS — waiting (${waited}s so far)"
    sleep 30; waited=$((waited + 30))
  done
}

real_build() {  # $1 tree $2 build $3 log
  local tree="$1" b="$2" log="$3" dh="$SELF_DIR/dev_heavy.sh"
  [ -x "$dh" ] || { say "dev_heavy.sh not found at $dh"; return 1; }
  wait_for_tokens
  say "configure $b (log $log)"
  "$dh" build cmake -S "$tree" -B "$b" -DCMAKE_BUILD_TYPE=RelWithDebInfo >>"$log" 2>&1 \
    || { say "CONFIGURE FAILED — tail of $log:"; tail -20 "$log" >&2; return 1; }
  say "build bezelc + bezel_tests -j$JOBS (dev_heavy, foreground; log $log)"
  "$dh" build cmake --build "$b" -j"$JOBS" --target bezelc bezel_tests >>"$log" 2>&1 \
    || { say "BUILD FAILED — tail of $log:"; grep -a -m20 'error:' "$log" >&2; tail -20 "$log" >&2; return 1; }
}

verify_build() {  # $1 build dir -> rc; every artefact a reader will run must be real
  local b="$1" bins=0 grp path rest core="$1/libbezel_core.a"
  [ -x "$b/bezelc" ] || { say "verify: $b/bezelc missing"; return 1; }
  [ -s "$b/tests/test_binaries.txt" ] || { say "verify: $b/tests/test_binaries.txt missing/empty"; return 1; }
  while IFS=$'\t' read -r grp path rest; do
    [ -n "$grp" ] || continue
    case "$path" in "$b/"*) ;; *) say "verify: $grp binary $path is outside $b"; return 1 ;; esac
    [ -x "$path" ] || { say "verify: $path missing"; return 1; }
    { [ -f "$core" ] && [ "$path" -ot "$core" ]; } && { say "verify: $path is OLDER than libbezel_core.a"; return 1; }
    grep -qaF "$b/bezelc" "$path" || { say "verify: $path does not embed $b/bezelc — its baked paths point elsewhere"; return 1; }
    [ -n "$(BEZEL_TEST_ENUMERATE=1 "$path" 2>/dev/null | head -1)" ] || { say "verify: $path enumerates no tests"; return 1; }
    bins=$((bins + 1))
  done < "$b/tests/test_binaries.txt"
  [ "$bins" -gt 0 ] || { say "verify: no test binaries listed"; return 1; }
}

write_manifest() {  # $1 dir $2 sha $3 seconds
  local d="$1" sha="$2" secs="$3" b="$1/tree/build" f grp path rest tc
  tc="$(clang++ --version 2>/dev/null | head -1)"
  {
    echo "format=1"
    echo "sha=$sha"
    echo "treekey=$(tree_key "$sha")"
    tree_components "$sha" | sed 's/^/tree./'
    echo "built_at=$(date -Iseconds)"
    echo "built_at_epoch=$(date +%s)"
    echo "built_at_ns=$(date +%s%N)"
    echo "build_seconds=$secs"
    echo "jobs=$JOBS"
    echo "host=$(hostname 2>/dev/null)"
    echo "toolchain=$tc"
    echo "built_by=$ROOT"
    echo "tree_dir=$d/tree"
    echo "build_dir=$b"
    for f in bezelc libbezel_core.a libbezel_runtime.a tests/test_binaries.txt; do
      [ -f "$b/$f" ] && echo "md5.$f=$(md5sum "$b/$f" | cut -d' ' -f1)"
    done
    while IFS=$'\t' read -r grp path rest; do
      [ -n "$grp" ] && echo "md5.tests/$(basename "$path")=$(md5sum "$path" | cut -d' ' -f1)"
    done < "$b/tests/test_binaries.txt"
  } > "$d/MANIFEST.tmp"
}

publish() {  # $1 sha -> prints the dir; rc
  local sha="$1" d="$SHARED_ROOT/$1" bfd t0 log rc
  is_sha "$sha" || die "publish: '$sha' is not a full lowercase 40-hex sha"
  [ -d "$SHARED_ROOT" ] || [ -d "$(dirname -- "$SHARED_ROOT")" ] \
    || die "publish: parent of shared root '$SHARED_ROOT' does not exist — refusing"
  mkdir -p "$LOCKS" || die "cannot create $LOCKS"
  exec {bfd}>"$LOCKS/$sha.build"
  if ! flock -n "$bfd"; then
    say "another publish of ${sha:0:12} is running — waiting for it"
    flock "$bfd"
  fi
  if complete_dir "$sha"; then
    say "already published: $d"
    exec {bfd}>&-; printf '%s\n' "$d"; return 0
  fi
  [ -e "$d" ] && { say "removing an earlier incomplete build of ${sha:0:12}"; remove_build_dir "$d"; }
  mkdir -p "$d" || die "cannot create $d"
  log="$d/publish.log"; : > "$log"
  t0="$(date +%s)"
  git -C "$ROOT" worktree add --detach "$d/tree" "$sha" >>"$log" 2>&1 \
    || { say "git worktree add failed (see $log)"; exec {bfd}>&-; return 1; }
  git -C "$ROOT" worktree lock --reason "shared build ${sha:0:12} (scripts/shared_build.sh)" "$d/tree" >>"$log" 2>&1 || true
  mkdir -p "$d/tree/build"
  if [ -n "${BEZEL_SHARED_BUILD_TEST_BUILDER:-}" ]; then
    SB_TREE="$d/tree" SB_BUILD="$d/tree/build" SB_SHA="$sha" \
      bash -c "$BEZEL_SHARED_BUILD_TEST_BUILDER" >>"$log" 2>&1; rc=$?
  else
    real_build "$d/tree" "$d/tree/build" "$log"; rc=$?
  fi
  [ "$rc" -eq 0 ] || { say "build of ${sha:0:12} FAILED — nothing published (partial dir $d kept for diagnosis; prune reaps it)"; exec {bfd}>&-; return 1; }
  verify_build "$d/tree/build" || { say "verification FAILED — nothing published"; exec {bfd}>&-; return 1; }
  ln -sfn tree/build/bezelc "$d/bezelc"
  cp "$d/tree/build/tests/test_binaries.txt" "$d/test_binaries.txt"
  write_manifest "$d" "$sha" "$(( $(date +%s) - t0 ))"
  chmod -R a-w "$d/tree"
  chmod a-w "$d/test_binaries.txt" "$d/MANIFEST.tmp"
  mv -f "$d/MANIFEST.tmp" "$d/MANIFEST" || { say "cannot write MANIFEST"; exec {bfd}>&-; return 1; }
  say "published ${sha:0:12} -> $d ($(mf_get "$d/MANIFEST" build_seconds)s)"
  exec {bfd}>&-
  printf '%s\n' "$d"
}

# --- commands -------------------------------------------------------------------
cmd_publish() {
  local ref="$DEFAULT_REF" keep="$KEEP" sha d
  while [ $# -gt 0 ]; do
    case "$1" in
      --ref) ref="${2:-}"; shift 2 ;;
      --keep) keep="${2:-}"; shift 2 ;;
      *) die "publish: unknown arg '$1'" ;;
    esac
  done
  sha="$(need_sha "$ref")"
  d="$(publish "$sha")" || exit 1
  cmd_prune --keep "$keep"
  printf '%s\n' "$d"
}

cmd_ensure() {
  [ $# -eq 1 ] || die "usage: shared_build.sh ensure REF"
  local sha d; sha="$(need_sha "$1")"
  if complete_dir "$sha"; then printf '%s\n' "$SHARED_ROOT/$sha"; return 0; fi
  d="$(publish "$sha")" || exit 1
  cmd_prune
  printf '%s\n' "$d"
}

cmd_path() {
  local by_tree=0 sha s
  [ "${1:-}" = "--tree" ] && { by_tree=1; shift; }
  [ $# -eq 1 ] || die "usage: shared_build.sh path [--tree] REF"
  sha="$(need_sha "$1")"
  if [ "$by_tree" = 1 ]; then
    s="$(find_by_key "$sha" "$(tree_key "$sha")")" || { say "no published build with ${1}'s tree key"; exit 1; }
  else
    complete_dir "$sha" || { say "no published build for $1 (${sha:0:12}); publish: scripts/shared_build.sh ensure $1"; exit 1; }
    s="$sha"
  fi
  warn_toolchain "$s"
  printf '%s\n' "$SHARED_ROOT/$s"
}

parse_wt() {  # sets WT and REST from [--worktree DIR] args...
  WT="$ROOT"; REST=()
  while [ $# -gt 0 ]; do
    case "$1" in
      --worktree) WT="${2:-}"; shift 2 ;;
      *) REST+=("$1"); shift ;;
    esac
  done
  [ -n "$WT" ] && git -C "$WT" rev-parse --show-toplevel >/dev/null 2>&1 || die "--worktree '$WT' is not a git worktree"
}

cmd_check_clean() {
  parse_wt "$@"; [ "${#REST[@]}" -eq 1 ] || die "usage: shared_build.sh check-clean [--worktree DIR] REF"
  local sha; sha="$(need_sha "${REST[0]}" "$WT")"
  check_clean "$WT" "$sha" "${REST[0]}"
}

cmd_resolve() {
  parse_wt "$@"; [ "${#REST[@]}" -eq 1 ] || die "usage: shared_build.sh resolve [--worktree DIR] REF|auto"
  local ref="${REST[0]}" label sha s match
  if [ "$ref" = auto ]; then ref=HEAD; label="HEAD (auto)"; else label="$ref"; fi
  sha="$(need_sha "$ref" "$WT")"
  check_clean "$WT" "$sha" "$label" || exit 3
  s="$(find_by_key "$sha" "$(tree_key "$sha")")" || {
    say "no published build for $label (${sha:0:12}) or any commit with the same src/tests tree."
    say "  Publish it first: scripts/shared_build.sh ensure $ref   (or build normally)"
    exit 1; }
  warn_toolchain "$s"
  if [ "$s" = "$sha" ]; then match=exact; else match=tree; fi
  echo "dir=$SHARED_ROOT/$s"
  echo "build=$(mf_get "$SHARED_ROOT/$s/MANIFEST" build_dir)"
  echo "lock=$LOCKS/$s.use"
  echo "sha=$s"
  echo "match=$match"
}

cmd_offer() {
  parse_wt "$@"
  [ -d "$SHARED_ROOT" ] || return 0
  local sha s
  sha="$(resolve_sha HEAD "$WT")" || return 0
  check_clean "$WT" "$sha" HEAD 2>/dev/null || return 0
  s="$(find_by_key "$sha" "$(tree_key "$sha")")" || return 0
  say "NOTE: a published build matches this clean tree: $SHARED_ROOT/$s"
  say "  A read-only run can skip this build: scripts/agent_check.sh --use-shared auto <filters>"
  return 0
}

cmd_list() {
  local s m
  for s in $(published_shas); do
    m="$SHARED_ROOT/$s/MANIFEST"
    printf '%s  %s  treekey=%s  %ss\n' "$s" "$(mf_get "$m" built_at)" \
      "$(mf_get "$m" treekey | cut -c1-12)" "$(mf_get "$m" build_seconds)"
  done
}

sub="${1:-}"; shift || true
case "$sub" in
  publish)     cmd_publish "$@" ;;
  ensure)      cmd_ensure "$@" ;;
  path)        cmd_path "$@" ;;
  treekey)     [ $# -eq 1 ] || die "usage: shared_build.sh treekey REF"; tree_key "$(need_sha "$1")" ;;
  check-clean) cmd_check_clean "$@" ;;
  resolve)     cmd_resolve "$@" ;;
  offer)       cmd_offer "$@" ;;
  list)        cmd_list ;;
  prune)       cmd_prune "$@" ;;
  ""|-h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) die "unknown subcommand '$sub' (publish|ensure|path|treekey|check-clean|resolve|offer|list|prune)" ;;
esac
