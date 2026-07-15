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
    # Per-shard timeout: a DEADLOCKED test (e.g. a cross-domain-mutex contention
    # race, #1609) otherwise hangs `wait` below forever and wedges the whole gate.
    # timeout kills the hung shard (exit 124/137 -> counted as a crashed shard ->
    # the gate's flake arbitration re-runs it). A shard normally finishes in ~45s.
    #
    # Wrapped in a subshell purely to record the shard's OWN wall time (appended as
    # a SHARD_ELAPSED marker to its log, surfaced below) — observation only. The
    # timeout still DIRECTLY wraps $bin so hung-test kill semantics are unchanged,
    # and `exit $_rc` preserves the shard's exit code for the `wait` below.
    (
        _s=$(awk '{print $1; exit}' /proc/uptime 2>/dev/null || date +%s)
        TMPDIR="$shard_tmp" BEZEL_SHARD_COUNT="$shards" BEZEL_SHARD_INDEX="$i" \
            timeout -k 10 "${BEZEL_SHARD_TIMEOUT:-300}" "$bin" > "$logdir/shard.$i.log" 2>&1
        _rc=$?
        _e=$(awk '{print $1; exit}' /proc/uptime 2>/dev/null || date +%s)
        awk "BEGIN{printf \"SHARD_ELAPSED %.2f\\n\", $_e-$_s}" >> "$logdir/shard.$i.log"
        exit $_rc
    ) &
    pids+=("$!")
done

# Collect exit codes; a crashed shard (nonzero) is a hard failure.
shard_rc=0
crashed_shards=()
for i in "${!pids[@]}"; do
    if ! wait "${pids[$i]}"; then
        echo "shard $i exited nonzero (crash or test failure)" >&2
        shard_rc=1
        crashed_shards+=("$i")
    fi
done

# Aggregate the per-shard "N passed, M failed" lines. Each shard also prints the
# full registry size as "... T total" (identical across shards) — capture it as
# the EXPECTED count so a shard that dies mid-run (its passed count missing) is
# detected as an undercount rather than passing silently.
#
# summaries_seen ALSO records, EXPLICITLY, how many shards printed a completion
# summary at all. A shard that exits 0 yet never printed a summary — e.g. a
# residual in-process path that abort()s or exit()s the harness mid-loop — drops
# its whole slice while evading detection: crashed_shards misses it (exit 0), and
# the ran!=expected_total undercount check can only catch it when SOME OTHER
# shard managed to establish expected_total. This direct "every shard must
# complete" check closes that window unconditionally, and is the safety basis for
# the shard-authoritative gate (scripts/gate.sh, #1861) having no serial backstop.
# It is purely additive: a healthy run has summaries_seen == shards and prints
# nothing new below.
total_pass=0
total_fail=0
expected_total=0
summaries_seen=0
missing_summary_shards=()
for (( i = 0; i < shards; i++ )); do
    line="$(grep -E '^[0-9]+ passed, [0-9]+ failed' "$logdir/shard.$i.log" | tail -1)"
    if [[ -n "$line" ]]; then
        summaries_seen=$(( summaries_seen + 1 ))
    else
        missing_summary_shards+=("$i")
    fi
    p="$(sed -E 's/^([0-9]+) passed.*/\1/' <<<"$line")"
    f="$(sed -E 's/^[0-9]+ passed, ([0-9]+) failed.*/\1/' <<<"$line")"
    t="$(sed -E 's/.*, ([0-9]+) total.*/\1/' <<<"$line")"
    total_pass=$(( total_pass + ${p:-0} ))
    total_fail=$(( total_fail + ${f:-0} ))
    [[ "${t:-0}" -gt "$expected_total" ]] && expected_total="$t"
done
ran=$(( total_pass + total_fail ))

end=$(date +%s)

# Surface any failing test names across all shards.
fail_lines="$(grep -hE '^\[ FAIL \]' "$logdir"/shard.*.log || true)"
if [[ -n "$fail_lines" ]]; then
    echo "--- failing tests ---"
    echo "$fail_lines"
fi

# GATE INTEGRITY: a crashed shard (uncaught exception -> runtime exit(70), or a
# segfault) kills the host process and silently drops its remaining tests. Detect
# it two ways — a nonzero shard exit AND a count that falls short of the registry
# total — and fail LOUDLY, naming the crashed shard and the last test it ran
# (the crash culprit). Without this the summary reads "0 failed" on an undercount.
count_ok=1
if [[ "$expected_total" -gt 0 && "$ran" -ne "$expected_total" ]]; then
    count_ok=0
fi
# EVERY shard must have printed a completion summary. A shard missing one
# validated NONE of its slice (it died before the summary), so a "0 failed"
# aggregate is not trustworthy regardless of exit code — this is stricter than
# the undercount check (which needs another shard to have set expected_total) and
# never lets a dropped slice pass green.
summary_ok=1
if [[ "$summaries_seen" -ne "$shards" ]]; then
    summary_ok=0
fi
if [[ "${#crashed_shards[@]}" -gt 0 || "$count_ok" -eq 0 || "$summary_ok" -eq 0 ]]; then
    echo "############################################################"
    echo "## GATE FAILURE: shard crash / undercount — result is NOT trustworthy"
    echo "##   ran $ran of $expected_total registered tests ($(( expected_total - ran )) unaccounted)"
    if [[ "$summary_ok" -eq 0 ]]; then
        echo "##   $(( shards - summaries_seen )) of $shards shards printed NO completion summary (slice not validated): shard(s) ${missing_summary_shards[*]}"
    fi
    for i in "${crashed_shards[@]}"; do
        last="$(grep -E '^\[ RUN' "$logdir/shard.$i.log" | tail -1 | sed -E 's/^\[ RUN  \] //')"
        echo "##   shard $i exited nonzero — died on: ${last:-<none>}"
    done
    echo "############################################################"
fi

# Per-shard wall times (observation only — lets the gate's timing log spot shard
# imbalance). Harvested from each shard's SHARD_ELAPSED marker before cleanup.
for (( i = 0; i < shards; i++ )); do
    et="$(grep -oE 'SHARD_ELAPSED [0-9.]+' "$logdir/shard.$i.log" 2>/dev/null | awk '{print $2}' | tail -1)"
    [[ -n "$et" ]] && echo "shard $i: ${et}s"
done

echo "============================================================"
echo "TOTAL: $total_pass passed, $total_fail failed  (across $shards shards, $((end - start))s)"
echo "============================================================"

if [[ "$shard_rc" -ne 0 || "$total_fail" -ne 0 || "$count_ok" -eq 0 || "$summary_ok" -eq 0 ]]; then
    exit 1
fi
exit 0
