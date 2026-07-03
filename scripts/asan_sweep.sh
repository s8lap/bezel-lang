#!/usr/bin/env bash
# Whole-suite ASan shard sweep (stage 0 of #1414). Builds (once) an ASan test
# binary in a scratch tree and runs all shards in parallel, reporting any
# AddressSanitizer errors. This is the gate that caught the v0.175 exception-
# layout heap corruption and the pre-existing #1390 — run it for merges that
# touch object layout, disposal, or the runtime.
#
# Usage: scripts/asan_sweep.sh [SHARDS=8]
set -u
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
shards="${1:-8}"
# Per-source-tree scratch (#1429): concurrent sweeps from different worktrees
# previously fought over one tree (and pre-d680e6c could silently gate another
# worktree's sources); the path hash gives each tree its own scratch.
scratch=/var/tmp/bezel-scratch/build-asan-$(echo -n "$repo" | cksum | cut -d' ' -f1)
# A cached tree configured against a different source dir (e.g. an agent
# worktree from a prior run) makes the reconfigure fail and the build silently
# rebuild the STALE tree — the sweep then "passes" on the wrong sources. Wipe
# on mismatch.
if [ -f "$scratch/CMakeCache.txt" ] && \
   ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=$repo\$" "$scratch/CMakeCache.txt"; then
  echo "asan_sweep: stale build tree (different source dir) — wiping $scratch"
  rm -rf "$scratch"
fi
mkdir -p "$scratch"
cmake -B "$scratch" -S "$repo" -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" > /dev/null || {
    echo "asan_sweep: cmake configure FAILED"; exit 2; }
# -j10, not -j14: a FRESH full ASan build (post stale-tree wipe) at -j14
# exceeds the dev_heavy memory cage and gets SIGTERM'd; incremental rebuilds
# never showed it because most TUs were cached. Override: ASAN_SWEEP_JOBS.
"$repo/scripts/dev_heavy.sh" build cmake --build "$scratch" -j"${ASAN_SWEEP_JOBS:-10}" --target bezel_tests || exit 2
fail=0
pids=()
for (( i=0; i<shards; i++ )); do
  d=/var/tmp/bezel-scratch/asan_sweep.$i; mkdir -p "$d"
  ( ASAN_OPTIONS=detect_leaks=0 TMPDIR="$d" BEZEL_SHARD_COUNT="$shards" BEZEL_SHARD_INDEX="$i" \
      "$scratch/tests/bezel_tests" > "$d/log" 2>&1 ) &
  pids+=($!)
done
for p in "${pids[@]}"; do wait "$p" || true; done
for (( i=0; i<shards; i++ )); do
  n=$(grep -c "ERROR: AddressSanitizer" "/var/tmp/bezel-scratch/asan_sweep.$i/log" || true)
  echo "shard $i: $n ASan errors"
  [ "$n" -gt 0 ] && { fail=1; grep -m1 -A15 "ERROR: AddressSanitizer" "/var/tmp/bezel-scratch/asan_sweep.$i/log"; }
done
exit $fail
