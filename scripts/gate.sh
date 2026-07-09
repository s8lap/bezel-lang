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
  # Collect ALL suspects: shard CRASHES (died on: a JIT'd program killed its host)
  # AND assertion FAILs. BOTH can be flaky under sharding — a crash, or a FAIL
  # from test-pollution (a polluter leaves global state; the round-robin lands it
  # with a snapshot/leak test). ARBITRATE uniformly: re-run each suspect's SUITE
  # alone up to 5x. A DETERMINISTIC failure (real regression / over-rejection)
  # fails all 5; a pollution/timing flake passes at least once. One run isn't
  # enough — it shares the flake rate. On --full the serial suite (below) is the
  # authoritative full-order re-check; a --quick pass-alone flake is accepted.
  crashed=$(grep 'died on:' "$S/gate_shard.log" | sed -E 's/.*died on: //')
  failed=$(grep -E '^\[ FAIL' "$S/gate_shard.log" | sed -E 's/^\[ FAIL \] //' | awk '{print $1}')
  suspects=$(printf '%s\n%s\n' "$crashed" "$failed" | grep -v '^$' | sort -u)
  [ -z "$suspects" ] && fail "sharded suite — nonzero exit, no crash/FAIL marker (see $S/gate_shard.log)"
  nsus=$(printf '%s\n' "$suspects" | grep -c .)
  # >8 distinct failing suites is a real regression, not pollution flakes; fail
  # fast (and bound the arbitration cost).
  if [ "$nsus" -gt 8 ]; then
    grep -E '^\[ FAIL' "$S/gate_shard.log" | head; fail "sharded suite — $nsus failing tests (real regression, too many to be flakes)"
  fi
  real=0
  while IFS= read -r t; do
    [ -z "$t" ] && continue
    suite="${t%%.*}"
    passed_alone=0
    for k in 1 2 3 4 5; do
      if BEZEL_TEST_FILTER="$suite" timeout 180 build/tests/bezel_tests >/dev/null 2>&1; then passed_alone=1; break; fi
    done
    if [ "$passed_alone" -eq 1 ]; then
      echo "   FLAKY: $t — its suite PASSES alone within 5 tries (sharding pollution/timing, not a deterministic failure)"
    else
      echo "   REAL: $t — fails alone all 5 tries"; real=1
    fi
  done <<< "$suspects"
  [ "$real" -eq 1 ] && fail "sharded suite — a suspect fails alone all 5 tries (real regression)"
  echo "   all sharded suspects flaky (pass alone) -> continuing to serial (authoritative)"
fi

if [ "$MODE" != "--quick" ]; then
  echo "== gate[4/*]: serial suite (heap-order, catches sharded-hidden fails) =="
  # A flaky-crash test (rc=70 mid-run) aborts the serial binary with NO summary
  # line and NO '[ FAIL' — which the old grep passed silently. Require a COMPLETE
  # green summary; retry up to 3x if the run didn't complete (flake); hard-fail on
  # any assertion FAIL immediately, or on a persistent no-complete (real crash).
  serial_ok=0
  for attempt in 1 2 3; do
    # timeout guards against a DEADLOCKED test (#1609 contention race) hanging the
    # serial binary forever; a hang leaves no clean summary -> treated as "did not
    # complete" -> retried, like a flaky crash.
    timeout -k 30 900 "$DH" suite build/tests/bezel_tests >"$S/gate_serial.log" 2>&1 || true
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
  "$DH" suite scripts/asan_sweep.sh >"$S/gate_asan.log" 2>&1; asan_rc=$?
  # asan_sweep exits 2 on configure/build failure. Previously `|| true` masked
  # that: a failed ASan build leaves no "ERROR: AddressSanitizer" markers, so the
  # error count below was 0 and the gate FALSE-PASSED having validated nothing.
  if [ "$asan_rc" -eq 2 ]; then
    echo "   asan_sweep: build/configure FAILED (exit 2):"; tail -20 "$S/gate_asan.log"; fail "asan_sweep — ASan build failed (validated nothing)"
  fi
  # Count REAL sanitizer errors — the per-shard summary line is "shard N: X ASan
  # errors", so grepping the substring 'ASan error' false-counts the zero lines.
  # Sum the per-shard counts, and also catch the sanitizer's own error markers.
  errs=$(awk 'match($0,/ASan errors/){for(i=1;i<=NF;i++)if($i=="ASan"){s+=$(i-1)}} END{print s+0}' "$S/gate_asan.log")
  errs=$(( errs + $(grep -cE '(ERROR|SUMMARY): (Address|Leak)Sanitizer|runtime error:|detected memory leaks' "$S/gate_asan.log") ))
  shards_reported=$(grep -c 'ASan errors' "$S/gate_asan.log")
  echo "   ASan shards reported: $shards_reported/8, real errors: $errs"
  # Enforce that all 8 shards actually ran — fewer means some silently skipped
  # (partial build failure / crash before reporting), which previously let the
  # gate pass having validated fewer than the full suite under ASan.
  if [ "$shards_reported" -ne 8 ]; then
    echo "   asan_sweep: only $shards_reported/8 shards reported (incomplete sweep):"; tail -20 "$S/gate_asan.log"; fail "asan_sweep — incomplete ($shards_reported/8 shards)"
  fi
  [ "$errs" -ne 0 ] && { grep -iE 'ERROR: AddressSanitizer|runtime error:|shard [0-9]+: [1-9]' "$S/gate_asan.log" | head; fail "asan_sweep"; }
fi

echo "== GATE PASS =="
