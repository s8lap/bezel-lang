#!/usr/bin/env bash
# Full integration gate for the CURRENT working tree — the blessed way to
# validate a merge before pushing. Runs synchronously and exits 0 iff every
# requested gate passes, so callers can `nohup scripts/gate.sh ... &` and rely on
# a single completion signal instead of ad-hoc pgrep/log waiters (which have
# self-matched and deadlocked).
#
# It encodes the disciplines learned the hard way (see CLAUDE.md + the
# build-test-efficiency notes):
#   1. RECONFIGURE first — CMake globs test files at configure time; a merge that
#      ADDS a new test *.cpp is invisible to `cmake --build` alone (the suite
#      count silently stays flat).
#   2. BUILD bezel_tests explicitly — run_parallel.sh only RUNS the binary, never
#      builds it; skipping this re-shards a STALE binary and reports the old count.
#   3. SHARDED suite via run_parallel.sh, which now FAILS LOUDLY on a shard crash
#      / undercount (a test whose JIT'd program throws uncaught -> runtime exit(70)
#      kills the whole shard host, silently dropping ~1/16 of tests).
#   4. SERIAL suite (heap-order) — sharded-green has masked heap corruption the
#      serial order exposed; also catches assertion FAILs that a crashed shard hid.
#   5. ASAN sweep — only for disposal/codegen/runtime/layout changes (--full).
# All heavy ops go through scripts/dev_heavy.sh (lock + memory cage).
#
# Usage:
#   scripts/gate.sh            # reconfigure + build + shard + serial  (default)
#   scripts/gate.sh --full     # + asan_sweep   (disposal/codegen/runtime batches)
#   scripts/gate.sh --quick    # reconfigure + build + shard only (sema-only checks)
set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gate: not in a git repo" >&2; exit 2; }
MODE="${1:-default}"
S=/var/tmp/bezel-scratch; mkdir -p "$S"
DH=scripts/dev_heavy.sh
fail() { echo "== GATE FAIL: $1 =="; exit 1; }

echo "== gate[1/*]: reconfigure (glob any new test files) =="
"$DH" build cmake -S . -B build >"$S/gate_reconf.log" 2>&1 || { tail -15 "$S/gate_reconf.log"; fail "configure"; }

echo "== gate[2/*]: build bezel_tests =="
"$DH" build cmake --build build --target bezel_tests -j16 >"$S/gate_build.log" 2>&1 \
  || { grep -iE 'error:|FAILED:' "$S/gate_build.log" | head -20; fail "build"; }
echo "   binary: $(ls -la --time-style=+%H:%M:%S build/tests/bezel_tests | awk '{print $6}')"

echo "== gate[3/*]: sharded suite (crash/undercount-checked) =="
"$DH" suite tests/run_parallel.sh >"$S/gate_shard.log" 2>&1; rc=$?
grep -E 'TOTAL:|## GATE FAILURE|died on|--- failing' "$S/gate_shard.log" || true
[ "$rc" -ne 0 ] && fail "sharded suite (crash / failures / undercount — see $S/gate_shard.log)"

if [ "$MODE" != "--quick" ]; then
  echo "== gate[4/*]: serial suite (heap-order, catches sharded-hidden fails) =="
  "$DH" suite build/tests/bezel_tests >"$S/gate_serial.log" 2>&1 || true
  grep -E '[0-9]+ passed, [0-9]+ failed, [0-9]+ total' "$S/gate_serial.log" | tail -1
  if grep -qE '^\[ FAIL' "$S/gate_serial.log"; then
    echo "   serial FAILs:"; grep -E '^\[ FAIL' "$S/gate_serial.log"; fail "serial suite"
  fi
fi

if [ "$MODE" = "--full" ]; then
  echo "== gate[5/*]: asan_sweep (disposal/codegen/runtime) =="
  # via dev_heavy suite: the lock is re-entrant, so asan_sweep's inner dev_heavy
  # build does not deadlock; do NOT run asan_sweep bare + a build concurrently.
  "$DH" suite scripts/asan_sweep.sh >"$S/gate_asan.log" 2>&1 || true
  errs=$(grep -icE 'ASan error|runtime error|LeakSanitizer' "$S/gate_asan.log")
  echo "   ASan shards reported: $(grep -c 'ASan errors' "$S/gate_asan.log")/8, error lines: $errs"
  [ "$errs" -ne 0 ] && { grep -iE 'ASan error|runtime error' "$S/gate_asan.log" | head; fail "asan_sweep"; }
fi

echo "== GATE PASS =="
