#!/usr/bin/env bash
#
# agent_check.sh — the ONE build+test command a producer/fix lane should use.
#
# Why this exists: a lane must build and run a TARGETED test subset — resource-
# capped, foreground, and WITHOUT the footguns that cost real time in practice:
#
#   * running the FULL suite (tests/run_parallel.sh / all of bezel_tests) — that
#     is the integration GATE's job; a lane running it spawns ~100 fork+link+exec
#     tasks and only wastes the shared budget.
#   * `--gtest_filter` — the test binary IGNORES it (see tests/test_main.cpp) and
#     silently runs the WHOLE suite.
#   * background build-waiters — `dev_heavy build` is already foreground/blocking;
#     a lane that "arms a waiter and yields" just stalls.
#   * a NEW test *.cpp not being picked up — CMake globs tests at CONFIGURE time,
#     so `cmake --build` alone can't see it. This wrapper reconfigures first.
#   * stray probe outputs (bezelc -o <name>) landing in the repo root.
#
# This wrapper encodes the correct protocol so a lane runs ONE command and cannot
# get it wrong. Resource-capping is delegated to scripts/dev_heavy.sh (the shared
# bezel-heavy.slice cgroup that bounds TOTAL cpu+mem across all concurrent lanes
# without serializing) — this script adds NO lock of its own.
#
# Usage:
#   scripts/agent_check.sh <filter> [filter2 ...]    build bezel_tests + run ONLY
#                                                     BEZEL_TEST_FILTER=<filter>
#                                                     (substring match on gtest
#                                                     suite-or-name; NOT the file
#                                                     basename). One or more.
#   scripts/agent_check.sh --build-only               reconfigure + build bezel_tests
#   scripts/agent_check.sh --probe FILE.bzl [args..]  compile ONE .bzl with bezelc
#                                                     (output -> the scratch dir)
#   scripts/agent_check.sh --probe --asan FILE.bzl    compile+RUN one .bzl under
#                                                     ASan/LSan with a hang timeout
#                                                     (for memory/disposal fixes)
#
# It NEVER runs the full suite. For whole-suite validation, that is the gate:
# scripts/gate.sh (coordinator only).
#
# Exit code: non-zero if the build fails or ANY requested filter has a failure.

set -uo pipefail

# --- locate the repo/worktree root (so this works from any agent worktree) ----
ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
  echo "agent_check: not inside a git worktree" >&2; exit 2; }
cd "$ROOT"
DH="$ROOT/scripts/dev_heavy.sh"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
JOBS="${JOBS:-16}"
SCRATCH="/var/tmp/bezel-scratch/agent-probe"
mkdir -p "$SCRATCH"

die() { echo "agent_check: $*" >&2; exit 2; }
[ -x "$DH" ] || die "scripts/dev_heavy.sh not found/executable at $DH"

# --- reconfigure (picks up any newly-added test *.cpp) + build a target -------
# Reconfigure is quiet unless it fails; the build hard-stops on failure so a
# stale binary can never be mistaken for a passing run.
reconfigure() {
  "$DH" build cmake -S "$ROOT" -B "$BUILD_DIR" >"$SCRATCH/agent_configure.log" 2>&1 \
    || { echo "agent_check: CONFIGURE FAILED (see $SCRATCH/agent_configure.log):" >&2
         tail -20 "$SCRATCH/agent_configure.log" >&2; exit 1; }
}
build_target() {  # $1 = cmake target ; $2 = binary to remove first (stale guard)
  local target="$1" bin="$2"
  [ -n "$bin" ] && rm -f "$bin"
  echo "== agent_check: reconfigure + build $target (dev_heavy, foreground) =="
  reconfigure
  "$DH" build cmake --build "$BUILD_DIR" -j"$JOBS" --target "$target" \
    || { echo "agent_check: BUILD FAILED ($target)" >&2; exit 1; }
  [ -z "$bin" ] || [ -x "$bin" ] || { echo "agent_check: $bin not produced" >&2; exit 1; }
}

# --- probe mode: compile (and optionally run under ASan) ONE .bzl -------------
if [ "${1:-}" = "--probe" ]; then
  shift
  asan=0; [ "${1:-}" = "--asan" ] && { asan=1; shift; }
  file="${1:-}"; shift || true
  [ -n "$file" ] && [ -f "$file" ] || die "--probe needs a .bzl file: agent_check.sh --probe [--asan] FILE.bzl"
  base="$(basename "${file%.bzl}")"
  BEZELC="$BUILD_DIR/bezelc"
  build_target bezelc "$BEZELC"
  if [ "$asan" = 1 ]; then
    out="$SCRATCH/${base}.asan.out"
    echo "== agent_check: probe --asan $file (output -> $out) =="
    "$DH" probe "$BEZELC" --sanitize=address -o "$out" "$file" "$@" \
      || { echo "agent_check: probe compile FAILED (see stderr above)" >&2; exit 1; }
    echo "== agent_check: running under ASan/LSan (20s hang timeout) =="
    "$DH" probe timeout 20 "$out"; rc=$?
    echo "agent_check: probe run exit=$rc  (124=timeout/hang, ASan reports above)"
    exit "$rc"
  else
    out="$SCRATCH/${base}.o"
    echo "== agent_check: probe compile $file -> $out =="
    "$DH" probe "$BEZELC" -c -o "$out" "$file" "$@"; exit $?
  fi
fi

# --- build-only ---------------------------------------------------------------
if [ "${1:-}" = "--build-only" ]; then
  build_target bezel_tests "$BUILD_DIR/tests/bezel_tests"
  echo "== agent_check: build OK =="
  exit 0
fi

# --- targeted test run (the default) ------------------------------------------
# Footgun guards BEFORE we build, so mistakes fail fast.
[ "$#" -ge 1 ] || die "no test filter given.
  Run a TARGETED subset:  scripts/agent_check.sh <filter> [filter2 ...]
  (whole-suite validation is the GATE's job: scripts/gate.sh — do NOT run it in a lane)"
for f in "$@"; do
  case "$f" in
    --gtest_filter*|--gtest*) die "the test binary IGNORES $f — pass a BARE substring, e.g. 'scripts/agent_check.sh my_area_1234'" ;;
    "") die "empty filter would run the WHOLE suite — give a real substring (the gate runs the full suite)" ;;
    run_parallel*|--full|--all) die "'$f' is a full-suite run — that is the gate's job, not a lane's" ;;
  esac
done

BIN="$BUILD_DIR/tests/bezel_tests"
build_target bezel_tests "$BIN"

rc_all=0
for f in "$@"; do
  echo "== agent_check: BEZEL_TEST_FILTER=$f (dev_heavy suite, capped) =="
  # dev_heavy suite runs in the shared slice; env is passed explicitly.
  "$DH" suite env BEZEL_TEST_FILTER="$f" "$BIN"; rc=$?
  if [ "$rc" -ne 0 ]; then echo "agent_check: FILTER '$f' had failures (exit $rc)"; rc_all=1
  else echo "agent_check: filter '$f' green"; fi
done
[ "$rc_all" -eq 0 ] && echo "== agent_check: ALL requested filters green ==" \
                    || echo "== agent_check: some filters FAILED (see above) =="
exit "$rc_all"
