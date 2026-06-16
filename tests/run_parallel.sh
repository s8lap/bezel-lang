#!/usr/bin/env bash
# Run the Bezel test suite in parallel shards.
#
# The test binary is a single executable whose harness (test_main.cpp) honours
# BEZEL_SHARD_COUNT / BEZEL_SHARD_INDEX to run a disjoint round-robin subset of
# the tests. Each shard is a separate process (its own LLVM/JIT context, ~314 MB
# RSS), so N shards across N cores run the whole suite in ~1/N the wall-clock of
# the serial run, while still executing every test.
#
# Usage:
#   tests/run_parallel.sh [SHARD_COUNT] [BINARY]
#     SHARD_COUNT  number of parallel shards (default: min(16, nproc))
#     BINARY       path to bezel_tests   (default: build/tests/bezel_tests)
#   BEZEL_TEST_FILTER=<substr> is passed through to each shard.
#
# Exit status is 0 only if every shard exited 0 and no test failed.
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="${2:-$repo_root/build/tests/bezel_tests}"
ncpu="$(nproc)"
default_shards=$(( ncpu < 16 ? ncpu : 16 ))
shards="${1:-$default_shards}"

if [[ ! -x "$bin" ]]; then
    echo "error: test binary not found/executable: $bin" >&2
    exit 2
fi

logdir="$(mktemp -d "${TMPDIR:-/tmp}/bezel_shards.XXXXXX")"
trap 'rm -rf "$logdir"' EXIT

echo "Running $bin across $shards shards (of $ncpu cores)..."
start=$(date +%s)

# Each shard gets its OWN TMPDIR. Many native/AOT tests write temp files with a
# fixed per-process stem (e.g. bezel_380_0.o) via std::filesystem::
# temp_directory_path(), which honours $TMPDIR. Without isolation two shards
# would write the same path concurrently and clobber each other; a private
# TMPDIR per shard makes those fixed stems collision-free with no test changes.
pids=()
for (( i = 0; i < shards; i++ )); do
    shard_tmp="$logdir/tmp.$i"
    mkdir -p "$shard_tmp"
    TMPDIR="$shard_tmp" BEZEL_SHARD_COUNT="$shards" BEZEL_SHARD_INDEX="$i" \
        "$bin" > "$logdir/shard.$i.log" 2>&1 &
    pids+=("$!")
done

# Collect exit codes; a crashed shard (nonzero) is a hard failure.
shard_rc=0
for i in "${!pids[@]}"; do
    if ! wait "${pids[$i]}"; then
        echo "shard $i exited nonzero (crash or test failure)" >&2
        shard_rc=1
    fi
done

# Aggregate the per-shard "N passed, M failed" lines.
total_pass=0
total_fail=0
for (( i = 0; i < shards; i++ )); do
    line="$(grep -E '^[0-9]+ passed, [0-9]+ failed' "$logdir/shard.$i.log" | tail -1)"
    p="$(sed -E 's/^([0-9]+) passed.*/\1/' <<<"$line")"
    f="$(sed -E 's/^[0-9]+ passed, ([0-9]+) failed.*/\1/' <<<"$line")"
    total_pass=$(( total_pass + ${p:-0} ))
    total_fail=$(( total_fail + ${f:-0} ))
done

end=$(date +%s)

# Surface any failing test names across all shards.
fail_lines="$(grep -hE '^\[ FAIL \]' "$logdir"/shard.*.log || true)"
if [[ -n "$fail_lines" ]]; then
    echo "--- failing tests ---"
    echo "$fail_lines"
fi

echo "============================================================"
echo "TOTAL: $total_pass passed, $total_fail failed  (across $shards shards, $((end - start))s)"
echo "============================================================"

if [[ "$shard_rc" -ne 0 || "$total_fail" -ne 0 ]]; then
    exit 1
fi
exit 0
