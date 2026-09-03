#!/usr/bin/env bash
# check_tidy.sh — clang-tidy static-analysis tier (ADVISORY; test-infra
# migration plan §2 / WS7:
# /var/tmp/bezel-scratch/impl-run-2026-07-12/aot-plan/ws7-static-analysis.md).
#
# Wraps run-clang-tidy over <build-dir>/compile_commands.json (requires
# CMAKE_EXPORT_COMPILE_COMMANDS ON — the CMakeLists.txt default) using the
# repo-root .clang-tidy config (clang-analyzer-* + bugprone-*, curated).
#
# Two compile-database caveats WS7 found, sanitized here before clang-tidy
# sees the database (report §1):
#  - CMake's native compile_commands.json carries the CMAKE_CXX_COMPILER_LAUNCHER
#    prefix (ccache, when present) as the first token of each `command`;
#    clang-tidy's driver detection wants the compiler itself first, so the
#    launcher token is stripped.
#  - Defensively drop any entry whose `file` ends `.o` or whose command
#    contains `&&` — a stray archive-compound entry `ninja -t compdb` produced
#    in WS7's run. CMake's native export hasn't shown this, but the filter is
#    cheap insurance against a future generator that does.
#
# Usage:
#   check_tidy.sh <build-dir>                        full sweep: every TU in
#                                                      the sanitized database
#   check_tidy.sh <build-dir> --changed [<base-ref>]  only changed src/*.cpp
#                                                      (default base: HEAD)
#   check_tidy.sh <build-dir> <file.cpp> [<file2.cpp> ...]
#                                                      explicit file list
#                                                      (regex-on-path, per
#                                                      run-clang-tidy)
#
# ADVISORY: prints findings and a count but always exits 0 unless
# clang-tidy/run-clang-tidy are simply absent from PATH (in which case it also
# exits 0 — the tier is "disabled", not "failed") — see .clang-tidy's header
# comment and ws7-static-analysis.md §2.5 for the promotion-to-blocking plan.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

BUILD_DIR="${1:-build}"
[ $# -ge 1 ] && shift

CLANG_TIDY="$(command -v clang-tidy || true)"
RUN_CLANG_TIDY="$(command -v run-clang-tidy || true)"
JQ="$(command -v jq || true)"
if [ -z "$CLANG_TIDY" ] || [ -z "$RUN_CLANG_TIDY" ] || [ -z "$JQ" ]; then
  echo "check-tidy: clang-tidy, run-clang-tidy, or jq not found on PATH — skipping (advisory tier disabled)."
  exit 0
fi

DB_SRC="$BUILD_DIR/compile_commands.json"
if [ ! -f "$DB_SRC" ]; then
  echo "check-tidy: $DB_SRC absent — reconfigure (CMAKE_EXPORT_COMPILE_COMMANDS is ON by default). Skipping."
  exit 0
fi

TIDY_DIR="$BUILD_DIR/tidy"
mkdir -p "$TIDY_DIR"
DB_OUT="$TIDY_DIR/compile_commands.json"

"$JQ" '[ .[]
      | select((.file // "") | endswith(".o") | not)
      | select((.command // ((.arguments // []) | join(" "))) | contains("&&") | not)
      | if .command then .command |= sub("^\\S*ccache\\S*\\s+"; "") else . end
    ]' "$DB_SRC" > "$DB_OUT" || { echo "check-tidy: failed to sanitize compile database — skipping"; exit 0; }

n_out=$("$JQ" 'length' "$DB_OUT")
n_src=$("$JQ" 'length' "$DB_SRC")
echo "check-tidy: sanitized database has $n_out TU(s) (from $n_src raw entries)"
if [ "$n_out" -eq 0 ]; then
  echo "check-tidy: sanitized database is empty — nothing to check."
  exit 0
fi

mode="${1:-}"
filter_args=()
case "$mode" in
  --changed)
    base="${2:-HEAD}"
    # fix #3718: print the base's short SHA. Nit (#3725, review of #3718):
    # gate_full_cycle.sh's caller already passes a resolved SHA here (the
    # output of its own `git merge-base`), not a moving ref — so the OLD log
    # already echoed a SHA back, not a ref name. What it never showed was
    # WHICH ref (`main` vs `origin/main` vs `HEAD~1`) that SHA was resolved
    # from, so a long-stale SHA from local `main` was indistinguishable from
    # a fresh one from `origin/main`. This line's own value is shortening a
    # full 40-char SHA for the log, plus resolving a genuine REF into a SHA
    # when a human runs `check_tidy.sh build --changed <ref>` by hand.
    base_sha="$(git rev-parse --short "$base" 2>/dev/null || echo "$base")"
    mapfile -t changed < <(git diff --name-only "$base" -- 'src/*.cpp' 'src/**/*.cpp' 2>/dev/null)
    if [ "${#changed[@]}" -eq 0 ]; then
      echo "check-tidy: no changed src/*.cpp vs $base ($base_sha) — nothing to check."
      exit 0
    fi
    # One alternation regex of absolute paths, matched against run-clang-tidy's
    # positional-arg regex-on-path (files not present in the DB just miss).
    regex=""
    for f in "${changed[@]}"; do
      abs="$ROOT/$f"
      esc=$(printf '%s' "$abs" | sed -e 's/[.[\*^$/]/\\&/g')
      regex="${regex:+$regex|}$esc"
    done
    filter_args=("$regex")
    echo "check-tidy: changed-files mode vs $base ($base_sha) — ${#changed[@]} file(s)"
    ;;
  --full|"")
    # Scope per WS7 report: the compiler's own C++ under src/ — NOT the
    # ~1033-TU bezel_tests suite, which also lands in compile_commands.json
    # (one DB covers every target). Without this filter "full sweep" would mean
    # the whole test corpus too, blowing past WS7's measured ~4-5 min/62-TU
    # estimate by an order of magnitude.
    esc_root=$(printf '%s' "$ROOT/src/" | sed -e 's/[.[\*^$/]/\\&/g')
    filter_args=("$esc_root")
    ;;
  *)
    filter_args=("$@")
    ;;
esac

jobs="${BEZEL_TIDY_JOBS:-8}"
LOG="$TIDY_DIR/run.log"
echo "check-tidy: running run-clang-tidy (-j $jobs)..."
"$RUN_CLANG_TIDY" -p "$TIDY_DIR" -config-file "$ROOT/.clang-tidy" -quiet -j "$jobs" \
  "${filter_args[@]}" > "$LOG" 2>&1
rc=$?

findings=$(grep -cE '^[^ ].*:[0-9]+:[0-9]+: (warning|error):' "$LOG" || true)
echo "check-tidy: $findings finding(s) (ADVISORY — does not fail the build; WS7 measured ~70-80% FP on this codebase)"
if [ "$findings" -gt 0 ]; then
  grep -E '^[^ ].*:[0-9]+:[0-9]+: (warning|error):' "$LOG" | sed -E 's/^/  /' | head -50
  [ "$findings" -gt 50 ] && echo "  ... ($((findings - 50)) more; full log: $LOG)"
fi
if [ "$rc" -ne 0 ] && [ "$findings" -eq 0 ]; then
  echo "check-tidy: run-clang-tidy exited $rc with no parsed findings — possible tool/config failure, see $LOG"
  tail -20 "$LOG"
fi
exit 0   # advisory: never fails the caller (WS7 report §2.5 — burn down first, then flip to blocking)
