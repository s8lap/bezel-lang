#!/usr/bin/env bash
# Whole-suite ASan sweep (#1822). Builds bezel_tests with NORMAL flags, then runs
# the whole suite with BEZEL_AOT_FORCE_SANITIZE=address so EVERY AOT test program
# is compiled with generated-code AddressSanitizer (opts.sanitizeAddress — the
# proven runAotChecked recipe: LLVM's AddressSanitizerPass over the emitted module
# + `-fsanitize=address -g` link against the plain runtime archive) and run under
# ASan. A memory error (use-after-free, out-of-bounds READ, double-free) in ANY
# test program prints an "ERROR: AddressSanitizer" report that the harness
# re-emits to the shard log; this sweep greps for it and FAILS. This is the gate
# that caught the v0.175 exception-layout heap corruption and the pre-existing
# #1390 — run it for merges that touch object layout, disposal, or the runtime.
#
# Pre-#1822 this rebuilt bezel_tests itself with CMAKE `-fsanitize=address`, i.e.
# it instrumented the C++ HARNESS, not the generated Bezel code. Under the AOT
# harness each program is linked per-test with plain `clang`, so no program ran
# under ASan and the grep never matched → the sweep reported "0 errors" having
# validated NOTHING (false-green). See issue #1822.
#
# LSan (leak detection) is deliberately OFF suite-wide (the harness runs the
# forced programs with detect_leaks=0): whole-suite leak detection is too noisy —
# negative tests that exit via an uncaught exception legitimately leave heap live
# at exit. Per-test LSan coverage stays on the curated runAotChecked call-sites
# (leak-clean by construction), which this sweep also honours.
#
# Usage: scripts/asan_sweep.sh [SHARDS=8]
set -u
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
shards="${1:-8}"
build="$repo/build"

# Build bezel_tests with NORMAL flags — NOT -fsanitize=address on the C++. #1822:
# the generated Bezel PROGRAM is ASan-instrumented per-test by the harness, so the
# harness + runtime archive stay a plain build (exactly what runAotChecked links
# against today). Reuse the worktree's own build/ tree: when invoked from gate.sh
# it is already configured+built, so this is a fast incremental no-op; standalone
# it configures (matching gate.sh's `cmake -S . -B build`) and builds.
if [ ! -f "$build/CMakeCache.txt" ]; then
  "$repo/scripts/dev_heavy.sh" build cmake -S "$repo" -B "$build" > /dev/null 2>&1 || {
    echo "asan_sweep: cmake configure FAILED"; exit 2; }
fi
"$repo/scripts/dev_heavy.sh" build cmake --build "$build" -j"${ASAN_SWEEP_JOBS:-16}" \
  --target bezel_tests || { echo "asan_sweep: build FAILED"; exit 2; }

bin="$build/tests/bezel_tests"
[ -x "$bin" ] || { echo "asan_sweep: test binary missing at $bin"; exit 2; }

# Run all shards in parallel. Each program compiles+links with ASan (per-test,
# via the harness) and runs under ASan; a report is re-emitted to the shard log.
# Per-shard TMPDIR keeps the harness's fixed-stem temp files collision-free
# (mirrors tests/run_parallel.sh). The compile concurrency is bounded machine-wide
# by the #1780 link-token pool, so 8 shards do not oversubscribe fork/RAM.
fail=0
pids=()
for (( i=0; i<shards; i++ )); do
  d=/var/tmp/bezel-scratch/asan_sweep.$i; mkdir -p "$d"; rm -f "$d/log"
  ( TMPDIR="$d" BEZEL_AOT_FORCE_SANITIZE=address \
      BEZEL_SHARD_COUNT="$shards" BEZEL_SHARD_INDEX="$i" \
      "$bin" > "$d/log" 2>&1 ) &
  pids+=($!)
done
for p in "${pids[@]}"; do wait "$p" || true; done

# Detect a real memory error via the sanitizer's own report headline in the shard
# log (the harness re-emits any fired report there). Print one report per dirty
# shard for triage. The per-shard "shard N: X ASan errors" line is the format
# gate.sh's asan tier sums, and printing exactly `shards` of them lets gate.sh's
# "all shards reported" integrity check pass.
for (( i=0; i<shards; i++ )); do
  log=/var/tmp/bezel-scratch/asan_sweep.$i/log
  n=0
  # grep -c already prints 0 (exit 1) when there is no match; do NOT add a `|| echo
  # 0` fallback — that emits a SECOND "0" and splits the report line.
  [ -s "$log" ] && n=$(grep -cE 'ERROR: (Address|Leak)Sanitizer' "$log" 2>/dev/null)
  n=${n:-0}
  # A shard that did not print the harness completion summary died mid-run (a
  # residual in-process JIT test can corrupt/abort the harness), so its slice was
  # NOT fully validated — a "0" there is not trustworthy. Treat an incomplete
  # shard as a failure, mirroring run_parallel.sh's undercount discipline.
  if ! grep -qE '[0-9]+ passed, [0-9]+ failed' "$log" 2>/dev/null; then
    echo "shard $i: ${n} ASan errors (INCOMPLETE — no completion summary; slice not fully swept)"
    fail=1
    [ "${n:-0}" -gt 0 ] && grep -m1 -B2 -A20 -E 'ERROR: (Address|Leak)Sanitizer' "$log"
    continue
  fi
  echo "shard $i: ${n} ASan errors"
  if [ "${n:-0}" -gt 0 ]; then
    fail=1
    grep -m1 -B2 -A20 -E 'ERROR: (Address|Leak)Sanitizer' "$log"
  fi
done
exit $fail
