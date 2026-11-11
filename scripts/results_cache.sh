#!/usr/bin/env bash
#
# results_cache.sh — compute the KEY FILE the test harness's results cache reads
# (tests/support/results_cache.hpp, wired into tests/test_main.cpp), and keep the
# cache directory tidy. scripts/agent_check.sh is the only caller that runs tests
# with a key file; nothing in the gate ever does (every gate script and
# tests/run_parallel.sh export BEZEL_RESULTS_CACHE=0, and this script then
# refuses to emit a key).
#
# WHAT A KEY COVERS. A test's recorded PASS may be served only when nothing that
# can change its verdict differs. Coarse on purpose — same inputs is a hit,
# anything else is a miss, and there is no cross-commit reuse:
#   * the sha256 of the test binary (it embeds the test source, the assertions
#     and the whole in-process compiler), tied to its exact stat identity
#     (dev:ino:size:mtime_ns, read before AND after hashing), which the harness
#     re-checks against /proc/self/exe, so a key is never applied to a binary
#     rebuilt since. The stat id lives in the key-file LINE, not in the hashed
#     components: a byte-identical relink (same sha256, new mtime) gets a fresh
#     line but the SAME key, so it keeps its recorded passes;
#   * the sha256 of the artefacts it embeds the ABSOLUTE paths of (asked of the
#     binary itself: BEZEL_RESULTS_CACHE_DESCRIBE): bezelc, libbezel_runtime.a,
#     libbezel_core.a — a bezelc-only or runtime-only rebuild is a miss;
#   * the source tree it reads from at run time (<repo>, <repo>/src/runtime):
#     the HEAD tree id, and the tree must be CLEAN (no tracked change, no
#     untracked file). A dirty tree gets NO key: work in progress and the
#     reviewer's fail-without arm (a temporary `git checkout <base> -- src/...`)
#     are therefore never cached, structurally;
#   * the toolchain: `clang --version`, and the identity (path, size, mtime,
#     inode) of clang, the linkers, every shared library the binary loads
#     (libLLVM.so, libstdc++, libc, ...) and clang's runtime libraries;
#   * the schema version.
# The harness then mixes in every behaviour-changing environment variable itself
# (results_cache.hpp envAffectsKey), so the environment it hashes is exactly the
# one the tests run under.
#
# Usage:
#   results_cache.sh keys OUT BIN [BIN...]
#       Append one key line per cacheable BIN to OUT (created if missing; a BIN
#       already in OUT is skipped). One status line per BIN on stderr. Exit 0
#       when every BIN got a key, 1 when at least one did not (the reason is on
#       stderr; the harness then runs that binary uncached).
#   results_cache.sh describe BIN     print BIN's key components (for debugging)
#   results_cache.sh prune [--max-age-days N]
#       Remove key directories untouched for N days (default 5). Run by
#       agent_check.sh at most once every 6 hours.
#   results_cache.sh stats            entry counts and size of the cache
#
# Env: BEZEL_RESULTS_CACHE (0/off: emit no key), BEZEL_RESULTS_CACHE_DIR
#      (default /mnt/fast/test-results-cache), BEZEL_RESULTS_CACHE_MIN_FREE_GB
#      (default 8: below that much free space on the cache's filesystem, emit no
#      key rather than fill the NVMe).

set -uo pipefail

SCHEMA="bezel.results-cache.v1"
DIR="${BEZEL_RESULTS_CACHE_DIR:-/mnt/fast/test-results-cache}"
MIN_FREE_GB="${BEZEL_RESULTS_CACHE_MIN_FREE_GB:-8}"

die() { echo "results_cache: $*" >&2; exit 2; }
case "${DIR%/}" in ""|/) die "BEZEL_RESULTS_CACHE_DIR must not be empty or / — refusing" ;; esac
DIR="${DIR%/}"

statid() { stat -L -c '%d:%i:%s:%.9Y' -- "$1" 2>/dev/null; }
sha() { sha256sum -- "$1" 2>/dev/null | cut -d' ' -f1; }
ident() {  # path -> "path size mtime inode" of the resolved file, or "path -"
  local r; r="$(readlink -f -- "$1" 2>/dev/null)" || r="$1"
  stat -L -c '%n %s %.9Y %i' -- "$r" 2>/dev/null || echo "$1 -"
}

mode_off_reason() {  # prints a reason and returns 0 when the cache is switched off
  case "${BEZEL_RESULTS_CACHE-}" in
    0|off|no|false) echo "BEZEL_RESULTS_CACHE=${BEZEL_RESULTS_CACHE}"; return 0 ;;
    ""|1|on|rw|yes|true|verify) return 1 ;;
    *) echo "unrecognised BEZEL_RESULTS_CACHE='${BEZEL_RESULTS_CACHE}' (fail closed)"; return 0 ;;
  esac
}

# The toolchain fingerprint, once per invocation.
TOOLCHAIN=""
toolchain() {
  [ -n "$TOOLCHAIN" ] && { printf '%s\n' "$TOOLCHAIN"; return; }
  local t="" c rd f
  t+="clang_version=$(clang --version 2>&1 | head -2 | paste -sd'|' -)"$'\n'
  for c in clang clang++ ld.lld mold ld; do
    f="$(command -v "$c" 2>/dev/null)" && t+="tool=$(ident "$f")"$'\n'
  done
  t+="tool=$(ident /usr/bin/clang)"$'\n'
  rd="$(clang -print-resource-dir 2>/dev/null)"
  if [ -n "$rd" ] && [ -d "$rd/lib" ]; then
    while IFS= read -r f; do t+="rt=$(ident "$f")"$'\n'; done \
      < <(find "$rd/lib" -maxdepth 2 -name 'libclang_rt.*' 2>/dev/null | LC_ALL=C sort)
  fi
  TOOLCHAIN="${t%$'\n'}"
  printf '%s\n' "$TOOLCHAIN"
}

# components BIN -> the key text on stdout (and rc 0), or a reason on stdout and rc 1.
components() {
  local bin="$1" d schema estat rroot bezelc rtlib corelib s0 s1 bsha st tree lib
  [ -x "$bin" ] || { echo "not an executable: $bin"; return 1; }
  # The identity probe. ENUMERATE + a filter matching nothing, so a binary that
  # predates the probe prints nothing and runs nothing.
  d="$(env BEZEL_RESULTS_CACHE_DESCRIBE=1 BEZEL_TEST_ENUMERATE=1 \
        BEZEL_TEST_FILTER=__results_cache_describe_matches_nothing__ BEZEL_TEST_LIST= \
        timeout 60 "$bin" 2>/dev/null)"
  get() { printf '%s\n' "$d" | sed -n "s/^$1=//p" | head -1; }
  schema="$(get schema)"
  [ "$schema" = "$SCHEMA" ] || { echo "binary predates the results cache (no '$SCHEMA' identity probe)"; return 1; }
  estat="$(get exe_stat)"; rroot="$(get repo_root)"
  bezelc="$(get bezelc)"; rtlib="$(get runtime_lib)"; corelib="$(get core_lib)"
  s0="$(statid "$bin")"
  [ -n "$s0" ] && [ "$s0" = "$estat" ] || { echo "stat id mismatch (binary replaced mid-probe?)"; return 1; }
  bsha="$(sha "$bin")"
  s1="$(statid "$bin")"
  [ -n "$bsha" ] && [ "$s1" = "$s0" ] || { echo "binary changed while it was being hashed"; return 1; }
  [ -d "$rroot" ] || { echo "embedded source root $rroot is missing"; return 1; }
  st="$(git -C "$rroot" status --porcelain=v1 --untracked-files=normal 2>/dev/null)" \
    || { echo "cannot read git status of $rroot"; return 1; }
  if [ -n "$st" ]; then
    echo "source tree $rroot has $(printf '%s\n' "$st" | wc -l) uncommitted/untracked path(s): results are cached only on a clean committed tree"
    return 1
  fi
  tree="$(git -C "$rroot" rev-parse 'HEAD^{tree}' 2>/dev/null)" || { echo "cannot resolve HEAD^{tree} in $rroot"; return 1; }
  printf 'schema=%s\n' "$SCHEMA"
  printf 'binary_sha256=%s\n' "$bsha"
  printf 'binary_stat=%s\n' "$s0"
  printf 'bezelc_sha256=%s\n' "$( [ -f "$bezelc" ] && sha "$bezelc" || echo -)"
  printf 'runtime_lib_sha256=%s\n' "$( [ -f "$rtlib" ] && sha "$rtlib" || echo -)"
  printf 'core_lib_sha256=%s\n' "$( [ -f "$corelib" ] && sha "$corelib" || echo -)"
  printf 'source_tree=%s\n' "$tree"
  while IFS= read -r lib; do
    [ -n "$lib" ] && printf 'lib=%s\n' "$(ident "$lib")"
  done < <(ldd "$bin" 2>/dev/null | awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}' | LC_ALL=C sort -u)
  toolchain
  # Paths are recorded for the reader, AFTER every content component; they are
  # part of the hash too (a different build dir is a different build).
  printf 'path.binary=%s\npath.bezelc=%s\npath.runtime_lib=%s\npath.core_lib=%s\npath.repo_root=%s\n' \
    "$bin" "$bezelc" "$rtlib" "$corelib" "$rroot"
  return 0
}

# The key of a component listing: its sha256 WITHOUT the binary_stat line. The
# content (binary_sha256) identifies the binary; the stat id only binds a key
# LINE to the file the harness will run (checked against /proc/self/exe).
keyof() { printf '%s\n' "$1" | grep -v '^binary_stat=' | sha256sum | cut -d' ' -f1; }

free_gb() {
  local p="$DIR"
  while [ ! -d "$p" ] && [ "$p" != "/" ]; do p="$(dirname "$p")"; done
  df -P -BG -- "$p" 2>/dev/null | awk 'NR==2 {gsub("G","",$4); print $4}'
}

cmd_keys() {
  local out="${1:-}"; shift || true
  [ -n "$out" ] || die "keys needs OUT and at least one BIN"
  [ "$#" -ge 1 ] || die "keys needs at least one BIN"
  local why rc=0 bin c key s fg tmp
  if why="$(mode_off_reason)"; then
    for bin in "$@"; do echo "results_cache: off for $(basename "$bin"): $why" >&2; done
    return 1
  fi
  fg="$(free_gb)"
  if [ -n "$fg" ] && [ "$fg" -lt "$MIN_FREE_GB" ]; then
    for bin in "$@"; do echo "results_cache: off for $(basename "$bin"): only ${fg}G free under $DIR (< ${MIN_FREE_GB}G)" >&2; done
    return 1
  fi
  mkdir -p "$DIR/artifacts" "$DIR/v1" || { echo "results_cache: off: cannot create $DIR" >&2; return 1; }
  TOOLCHAIN="$(toolchain)"   # once, before the per-binary subshells
  touch "$out" || die "cannot write $out"
  for bin in "$@"; do
    s="$(statid "$bin")"
    if [ -n "$s" ] && awk -F'\t' -v s="$s" '$2==s {f=1} END {exit !f}' "$out"; then continue; fi
    if ! c="$(components "$bin")"; then
      echo "results_cache: off for $(basename "$bin"): $c" >&2
      rc=1; continue
    fi
    key="$(keyof "$c")"
    tmp="$DIR/artifacts/.$key.$$.tmp"
    printf '%s\n' "$c" > "$tmp" && mv -f "$tmp" "$DIR/artifacts/$key.txt"
    printf '%s\t%s\t%s\t%s\n' "$SCHEMA" "$(printf '%s\n' "$c" | sed -n 's/^binary_stat=//p')" "$key" "$bin" >> "$out"
    echo "results_cache: key for $(basename "$bin"): ${key:0:12} (components: $DIR/artifacts/$key.txt)" >&2
  done
  return "$rc"
}

cmd_prune() {
  local days=5
  [ "${1:-}" = "--max-age-days" ] && { days="${2:-}"; shift 2; }
  case "$days" in ''|*[!0-9]*) die "--max-age-days needs a number" ;; esac
  [ -d "$DIR/v1" ] || return 0
  local d n=0
  while IFS= read -r d; do
    case "$(basename "$d")" in
      [0-9a-f]*) [[ "$(basename "$d")" =~ ^[0-9a-f]{64}$ ]] || continue ;;
      *) continue ;;
    esac
    rm -rf -- "$d" && n=$((n + 1))
  done < <(find "$DIR/v1" -mindepth 1 -maxdepth 1 -type d -mtime "+$days" 2>/dev/null)
  find "$DIR/artifacts" -maxdepth 1 -type f -name '*.txt' -mtime "+$days" -delete 2>/dev/null
  find "$DIR/artifacts" -maxdepth 1 -type f -name '.*.tmp' -mmin +60 -delete 2>/dev/null
  echo "results_cache: pruned $n key dir(s) older than ${days}d under $DIR/v1" >&2
}

cmd_stats() {
  [ -d "$DIR/v1" ] || { echo "results_cache: no cache at $DIR"; return 0; }
  local keys passes poisons
  keys="$(find "$DIR/v1" -mindepth 1 -maxdepth 1 -type d | wc -l)"
  passes="$(find "$DIR/v1" -mindepth 2 -maxdepth 2 -name '*.pass' | wc -l)"
  poisons="$(find "$DIR/v1" -mindepth 2 -maxdepth 2 -name '*.poison' | wc -l)"
  echo "results_cache: $DIR: $keys key(s), $passes pass entr(ies), $poisons poisoned, $(du -sh "$DIR" 2>/dev/null | cut -f1)"
}

case "${1:-}" in
  keys) shift; cmd_keys "$@" ;;
  describe) shift; [ -n "${1:-}" ] || die "describe needs a BIN"
            TOOLCHAIN="$(toolchain)"
            if c="$(components "$1")"; then printf '%s\n' "$c"; echo "key=$(keyof "$c")"
            else echo "off: $c"; exit 1; fi ;;
  prune) shift; cmd_prune "$@" ;;
  stats) cmd_stats ;;
  -h|--help|"") sed -n '2,52p' "$0"; [ -n "${1:-}" ] ;;
  *) die "unknown command '$1' (see --help)" ;;
esac
