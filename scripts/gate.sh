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
if [ "$rc" -ne 0 ]; then
  # An assertion FAIL is never a flake -> hard fail.
  grep -qE '^\[ FAIL' "$S/gate_shard.log" && fail "sharded suite — assertion failures"
  # Only shard CRASHES (a test's JIT'd program killed its host). Some are flaky
  # test-pollution under sharding (a test leaves global state; the round-robin
  # partition lands a polluter with a snapshot test). ARBITRATE: re-run each
  # crashed test alone; pass-alone => flake (continue to serial, the authority);
  # fail-alone => real crash => fail.
  crashed=$(grep 'died on:' "$S/gate_shard.log" | sed -E 's/.*died on: //')
  [ -z "$crashed" ] && fail "sharded suite — nonzero exit, no crash marker (see $S/gate_shard.log)"
  real=0
  # Re-run each crashed test's SUITE alone up to 5x. A DETERMINISTIC crash (real
  # regression) fails all 5; a LOAD/TIMING flake (e.g. cross-domain-mutex
  # contention tests, which crash a few % of the time under build load) passes
  # at least once. One isolation run is not enough — it shares the flake rate.
  while IFS= read -r t; do
    [ -z "$t" ] && continue
    suite="${t%%.*}"
    passed_alone=0
    for k in 1 2 3 4 5; do
      if BEZEL_TEST_FILTER="$suite" timeout 180 build/tests/bezel_tests >/dev/null 2>&1; then passed_alone=1; break; fi
    done
    if [ "$passed_alone" -eq 1 ]; then
      echo "   FLAKY: crashed test $t — its suite PASSES alone within 5 tries (load/timing flake, not a deterministic crash)"
    else
      echo "   REAL: crashed test $t — fails alone all 5 tries"; real=1
    fi
  done <<< "$crashed"
  [ "$real" -eq 1 ] && fail "sharded suite — a crashed test fails alone all 5 tries (real crash)"
  echo "   crashed-shard tests are flaky (pass alone) -> continuing to serial (authoritative)"
fi

if [ "$MODE" != "--quick" ]; then
  echo "== gate[4/*]: serial suite (heap-order, catches sharded-hidden fails) =="
  # A flaky-crash test (rc=70 mid-run) aborts the serial binary with NO summary
  # line and NO '[ FAIL' — which the old grep passed silently. Require a COMPLETE
  # green summary; retry up to 3x if the run didn't complete (flake); hard-fail on
  # any assertion FAIL immediately, or on a persistent no-complete (real crash).
  serial_ok=0
  for attempt in 1 2 3; do
    "$DH" suite build/tests/bezel_tests >"$S/gate_serial.log" 2>&1 || true
    if grep -qE '^\[ FAIL' "$S/gate_serial.log"; then
      echo "   serial FAILs (attempt $attempt):"; grep -E '^\[ FAIL' "$S/gate_serial.log"; fail "serial suite — assertion failure"
    fi
    if grep -qE '[0-9]+ passed, 0 failed, [0-9]+ total' "$S/gate_serial.log"; then
      grep -E '[0-9]+ passed, [0-9]+ failed, [0-9]+ total' "$S/gate_serial.log" | tail -1
      serial_ok=1; break
    fi
    echo "   serial run did NOT complete cleanly (no green summary — likely a flaky-test crash mid-run); retry $attempt/3"
  done
  [ "$serial_ok" -ne 1 ] && fail "serial suite — did not complete green in 3 attempts (persistent crash, not a flake)"
fi

if [ "$MODE" = "--full" ]; then
  echo "== gate[5/*]: asan_sweep (disposal/codegen/runtime) =="
  # via dev_heavy suite: the lock is re-entrant, so asan_sweep's inner dev_heavy
  # build does not deadlock; do NOT run asan_sweep bare + a build concurrently.
  "$DH" suite scripts/asan_sweep.sh >"$S/gate_asan.log" 2>&1 || true
  # Count REAL sanitizer errors — the per-shard summary line is "shard N: X ASan
  # errors", so grepping the substring 'ASan error' false-counts the zero lines.
  # Sum the per-shard counts, and also catch the sanitizer's own error markers.
  errs=$(awk 'match($0,/ASan errors/){for(i=1;i<=NF;i++)if($i=="ASan"){s+=$(i-1)}} END{print s+0}' "$S/gate_asan.log")
  errs=$(( errs + $(grep -cE '(ERROR|SUMMARY): (Address|Leak)Sanitizer|runtime error:|detected memory leaks' "$S/gate_asan.log") ))
  echo "   ASan shards reported: $(grep -c 'ASan errors' "$S/gate_asan.log")/8, real errors: $errs"
  [ "$errs" -ne 0 ] && { grep -iE 'ERROR: AddressSanitizer|runtime error:|shard [0-9]+: [1-9]' "$S/gate_asan.log" | head; fail "asan_sweep"; }
fi

echo "== GATE PASS =="
