#!/usr/bin/env bash
# Full integration gate for the CURRENT working tree — the blessed way to
# validate a merge before pushing. Runs synchronously and exits 0 iff every
# gate phase passes, so callers can `nohup scripts/gate.sh ... &` and rely on a
# single completion signal instead of ad-hoc pgrep/log waiters (which have
# self-matched and deadlocked).
#
# ONE gate, shard-authoritative (#1861). Since the JIT->AOT test migration every
# test's COMPILE runs in a fork()ed child and the generated program runs as a
# SEPARATE native process (tests/support/aot_support.hpp) — every test is
# process-isolated. The classic "serial-order heap corruption that a sharded-
# green run masked" is therefore STRUCTURALLY IMPOSSIBLE, so the always-on serial
# pass that used to guard exactly that class (~68% of the old gate's wall) is
# gone. The single hardened sharded pass (optionally ASan-instrumented, --full)
# is now the whole oracle:
#   1. RECONFIGURE first — CMake globs test files at configure time; a merge that
#      ADDS a new test *.cpp is invisible to `cmake --build` alone (the suite
#      count silently stays flat).
#   2. BUILD bezel_tests explicitly — run_parallel.sh only RUNS the binary, never
#      builds it; skipping this re-shards a STALE binary and reports the old count.
#   3. LIT diagnostic track — real bezelc + FileCheck, per-process.
#   4. SHARDED suite via run_parallel.sh — the authoritative correctness net. It
#      FAILS LOUDLY on a shard crash / undercount / missing-summary (drop
#      detection: a dropped or crashed slice must NEVER pass green). With --full
#      it also forces generated-code AddressSanitizer on every test program
#      (BEZEL_AOT_FORCE_SANITIZE=address) so the WHOLE suite runs under ASan in
#      the one pass — the former separate `asan_sweep` whole-suite sweep folded
#      in. A fired sanitizer report fails the gate deterministically.
# All heavy ops go through scripts/dev_heavy.sh (resource-capped slice).
#
# Modes (collapsed from default/--quick/--full/--fast — #1861):
#   scripts/gate.sh            # DEFAULT: reconfigure + build + lit + canary +
#                              #   tidy + ONE authoritative PLAIN sharded pass.
#                              #   NO serial. Fast + robust: plain compiles fit
#                              #   the per-shard timeout even cold and under
#                              #   multi-agent load.
#   scripts/gate.sh --full     # + whole-suite generated-code ASan folded INTO the
#                              #   sharded pass (delicate merges: object layout /
#                              #   disposal / runtime). Heavier — a COLD full-suite
#                              #   ASan compile can exceed the shard budget under
#                              #   heavy contention — so it is OPT-IN, not the
#                              #   default. (The role the old --full asan_sweep had,
#                              #   now unified into the one sharded runner.)
#   scripts/gate.sh --quick    # alias of the default (retained for callers)
#   scripts/gate.sh --no-asan  # alias of the default (explicit: no ASan)
# GATE_FAST_ASAN=1 forces ASan ON for any mode; GATE_FAST_ASAN=0 forces it OFF.
# (To make ASan the every-gate default on a quiet/uncontended host, run --full or
#  export GATE_FAST_ASAN=1 — see the ASAN block below for why it is not default.)
#
# CANARY tier (gate[4/6], #2100): before the authoritative sharded pass, the
# gate runs the diff-selected subset scripts/select_tests.py maps from the
# worktree's diff vs origin/main (issue-number cross-links, fine §N.M
# citations, downstream-dir fallback, diagnostic-string matches). Purpose:
# FAIL-FAST ONLY. Measured over 7 days of gate-timings, 24.8% of ALL gate wall
# was broken trees grinding every shard to the 300/900s timeout ceiling; a red
# canary fails the gate in minutes instead. A green canary proves NOTHING
# beyond its subset — the full sharded pass below remains the SOLE acceptance
# authority, unconditionally (the back-test on issue #2100: 2/9 historical
# regressions are structurally invisible to ANY diff-derived subset). Canary
# compiles populate the AOT bincache under the same key the full pass uses, so
# on a green gate the canary's cost is mostly amortized (the full pass serves
# those tests from cache, exec-only). The canary inherits the gate's ASan mode
# and uses the same flake arbitration as the full pass. Selector/infra errors
# SKIP the canary (fail-open to the full pass — the canary must never brick a
# gate on its own bugs). Knobs:
#   GATE_CANARY=0            skip the canary entirely
#   GATE_CANARY_BASE=<ref>   selection base (default: merge-base HEAD origin/main)
#   GATE_CANARY_MAX_PCT=<n>  skip when the selection exceeds n% of the suite
#                            (default 50 — the full pass then does the work anyway)
set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gate: not in a git repo" >&2; exit 2; }
# Scratch dir + verdict file path are set up BEFORE any preflight exit can fire,
# so the verdict writer (gate_write_verdict, below) has a path on every exit path
# (Phase 3, #3002). $S was previously set after the mode/builder preflights, so
# early exits (exit 64/2 below) could not write a verdict — now they can.
S=/var/tmp/bezel-scratch; mkdir -p "$S"
GATE_VERDICT_FILE="${GATE_VERDICT_FILE:-$S/gate-verdict.json}"
MODE="${1:-default}"

# ---------------------------------------------------------------------------
# gate_write_verdict — the authoritative verdict file writer (Phase 3, #3002).
#
# THE EXIT-CODE-TRAP CLASS: a backgrounded `gate_and_push.sh > log; echo EXIT=$?`
# reports the WRAPPER's exit, not the gate's. The gate's real verdict is smeared
# across ($?, banner greps, a status file) and they can disagree (#3002: rc=0
# over 31% coverage; #2633: rc=0 under an undercount banner). This writes ONE
# authoritative gate-verdict.json on EVERY exit, atomically, that Phase 3c
# consumers will read instead of $?/greps.
#
# PHASE 3 IS ADDITIVE: the write lands here, but NO consumer switches yet. The
# existing $?/banner/status-file outputs are UNCHANGED (back-compat). The file
# is observation-only until N gates prove it matches the gate's outcome.
#
# Best-effort: the `|| true` ensures a verdict-write hiccup NEVER changes the
# gate's exit code (the file is not load-bearing until Phase 3c switches a
# consumer to read it). per #3002 / the exit-code-trap.
# ---------------------------------------------------------------------------
gate_write_verdict() { # rc [total_s] — write gate-verdict.json; best-effort, never changes exit
  local rc="$1" total="${2:-0}"
  # Detect truncation: the "## GATE FAILURE: shard crash / undercount" banner
  # from run_parallel.sh:306 (Plan-A invariant 1.19: gate_full_cycle distrusts
  # rc=0+undercount-banner and retries) OR a coverage short (rc==3, #3002). The
  # truncated flag only turns an rc==0 pass into POISONED, but is recorded in
  # integrity for all exit codes so a consumer sees the full picture.
  local trunc=0 ubanner=0
  if [ -r "$S/gate_shard.log" ] && grep -q 'shard crash / undercount' "$S/gate_shard.log" 2>/dev/null; then
    trunc=1; ubanner=1
  fi
  [ "$rc" -eq 3 ] && trunc=1
  local oracle="plain"; [ "${ASAN:-0}" = "1" ] && oracle="asan"
  # per the exit-code-trap / #3002: --mode uses the `=` form because MODE can be
  # `--full`/`--quick`/`--no-asan`, and a bare `--mode --full` makes argparse
  # treat `--full` as an unknown FLAG (exit 2), silently skipping the verdict
  # write (the `|| true` below swallows it). That left the ASan oracle's verdict
  # unwritten for the entire Phase 3-additive window — the exact gap Phase 3c
  # closes by making the verdict load-bearing.
  local -a vargs=(--path "$GATE_VERDICT_FILE" --rc "$rc"
    --head "${GATE_HEAD:-}" --mode="${MODE:-}" --oracle "$oracle"
    --started-epoch "${GATE_EPOCH:-0}" --total-s "$total")
  local _fp="${FAILED_PHASE:-}"; [ -n "$_fp" ] && vargs+=(--failed-phase "$_fp")
  [ "$trunc" -eq 1 ] && vargs+=(--truncated)
  [ "$ubanner" -eq 1 ] && vargs+=(--undercount-banner)
  [ "${SHARD_COUNT:-0}" -gt 0 ] 2>/dev/null && vargs+=(--shard-count "$SHARD_COUNT")
  [ "${SHARD_REPORTED_S:-0}" -gt 0 ] 2>/dev/null && vargs+=(--shard-reported-s "$SHARD_REPORTED_S")
  # Coverage: pass only when the variable is SET (measured). Unset → omitted →
  # None in the JSON, per #3010 (an unmeasured number emits null, never 0, so a
  # harvest failure and a genuine zero stay distinguishable).
  [ -n "${COV_GTESTS+x}" ] && vargs+=(--coverage-gtests "$COV_GTESTS")
  [ -n "${COV_LITTESTS+x}" ] && vargs+=(--coverage-littests "$COV_LITTESTS")
  [ -n "${GATE_EXPECTED_GTESTS+x}" ] && vargs+=(--expected-gtests "$GATE_EXPECTED_GTESTS")
  [ -n "${ASAN_ERRS+x}" ] && vargs+=(--asan-errors "$ASAN_ERRS")
  [ -n "${ARB_SUSPECTS+x}" ] && vargs+=(--arbitration-suspects "$ARB_SUSPECTS")
  PYTHONPATH=scripts python3 -m gate.verdict write "${vargs[@]}" 2>/dev/null || true
}
# fix #3011: REJECT AN UNRECOGNISED MODE. `MODE` has exactly one semantic use
# (`[ "$MODE" = "--full" ] && ASAN=1`, below), so before this check ANY other
# argument ran a PLAIN gate while every log line, the status file, and the
# telemetry record labelled it whatever was typed. A typo did not fail — it
# silently ran the weaker oracle under the stronger name.
#
# That is not hypothetical. gate-timings.jsonl records six such runs:
#   --fast x4, --clean-first x2   (both --clean-first runs recorded result=pass)
# Each displaced a real oracle. `c7404bf` (#2917) made the DUAL-oracle rule
# structural five commits before this; a mistyped flag defeated it anyway.
#
# The allow-list is a POSITIVE list, which is variant 1 of this project's own
# category-defect taxonomy — it fails LOUD by rejecting a name it should know.
# That is the correct direction here: a new mode announces itself at the first
# invocation instead of silently degrading forever. Adding one is a one-word
# edit; add it to gate_and_push.sh's --single-oracle help too.
case "$MODE" in
  default|--quick|--full|--no-asan) ;;
  *)
    echo "gate: UNRECOGNISED MODE '$MODE'" >&2
    echo "  known modes: default | --quick | --no-asan (all plain) | --full (ASan)" >&2
    echo "  Refusing rather than running a plain gate under a name you did not mean." >&2
    gate_write_verdict 64 0
    exit 64 ;;
esac
DH=scripts/dev_heavy.sh

# Zero-builders preflight (cold-cache flake defense): a --full gate run
# concurrently with even one producer build can stall a shard and trigger the
# cold-cache timeout flake (the #1 documented cause). Refuse to start if heavy
# compile/test comms are already active. Matched by process COMM (exact name) so
# it cannot self-match this bash script. Bypass with GATE_SKIP_BUILDER_CHECK=1.
# NB `bezel_tests(_[a-z]+)?` covers BOTH the historical single binary and the
# per-area binaries the suite is now built as (bezel_tests_codegen, …) — pgrep's
# pattern is a regex and -x anchors it to the WHOLE comm, so this still cannot
# match this bash script.
BUILDERS='cmake|cc1plus|ld|mold|lld|ninja|make|bezel_tests(_[a-z]+)?'
if [ -z "${GATE_SKIP_BUILDER_CHECK:-}" ] && pgrep -x "$BUILDERS" >/dev/null 2>&1; then
  echo "GATE ABORT: heavy build/test process(es) already active — gating now risks the cold-cache flake (zero-builders rule). Stop them, or set GATE_SKIP_BUILDER_CHECK=1:" >&2
  pgrep -ax "$BUILDERS" >&2 || true
  gate_write_verdict 2 0
  exit 2
fi

# ASan is OPT-IN (--full), not the default (#1861). The unified ASan surface is
# still the sharded runner: with ASan on, the single sharded pass forces
# generated-code AddressSanitizer on every test program
# (BEZEL_AOT_FORCE_SANITIZE=address, honoured by runAot) so the WHOLE suite runs
# under ASan in ONE pass — the former separate asan_sweep folded in. It is NOT
# the default because a COLD full-suite ASan compile (the AOT bincache keys on
# the test-binary hash, so any relink is a full cache miss => cold) is far
# heavier than plain and, measured under the project's normal multi-agent
# contention, does not fit a sane per-shard timeout (two cold gate runs timed out
# at 300s AND 900s under ~load-19 with 40+ competing compiles). The plain default
# fits comfortably. Enable ASan for delicate merges (--full) or on a quiet host
# (GATE_FAST_ASAN=1); flipping the default here is a one-line change if the
# cold/contention cost is acceptable. The arbitration re-runs must force ASan too
# (gate_arbitrate's --asan flag) or an ASan-only failure would be falsely cleared
# as flaky.
ASAN=0
[ "$MODE" = "--full" ] && ASAN=1
case "${GATE_FAST_ASAN:-}" in
  1) ASAN=1 ;;
  0) ASAN=0 ;;
esac

# SHARD_ENV is spliced in as a COMMAND PREFIX at the run sites below, and
# dev_heavy.sh ends in `exec systemd-run ... -- "$@"` — systemd-run execs the
# first word after `--` as a FILENAME, so a bare `VAR=value` word there is not a
# shell assignment, it is a nonexistent program. The leading `env` is therefore
# load-bearing, and it is seeded here rather than in the ASan branch because as
# of #2918 the PLAIN pass also contributes a variable (its shard ceiling).
# ⚠ BEZEL_AOT_CACHE_STATS IS DELIBERATELY *NOT* SET HERE — see #3012.
#
# It was set here in the first draft of #3010, to get the AOT hit/miss numbers
# this project has never had. The gate caught the consequence immediately: the
# harness prints its `[aot-cache] …` line from an atexit hook that RACES the
# shard's own summary printer, and on sema shard 6 it landed mid-line:
#
#     102 passed[aot-cache] hits=5 misses=2 (71% hit) …
#     , 0 failed (shard 6/16), 1630 total
#
# run_parallel.sh matches `^[0-9]+ passed, [0-9]+ failed`, so a completed shard
# (rc=0, 102 real tests) was counted as 102 unaccounted and the whole gate failed.
# Enabling the measurement corrupted the thing being measured.
#
# The fix is to route the stats to their own file rather than into the stream the
# summary is parsed from (#3012, code change). Until then this stays off: an
# unmeasured number is a known gap, a corrupted count is a wrong answer.
SHARD_ENV=(env); SHARD_NOTE=""; SHARD_LABEL="crash/undercount-checked"
# An explicit caller-supplied BEZEL_SHARD_TIMEOUT wins unconditionally, in BOTH
# oracles (#2918 — this used to be read only in the ASan branch, so an operator
# exporting it for a plain gate had it silently ignored by this script and only
# honoured by run_parallel's own default-expansion).
GATE_EXPLICIT_STO="${BEZEL_SHARD_TIMEOUT:-}"
if [ "$ASAN" -eq 1 ]; then
  # The ASan per-shard ceiling is COLD-AWARE (#2100): a WARM ASan pass (k2
  # bincache populated) fits 900s with wide margin, but a COLD one — the first
  # after a compiler change, a fresh/evicted cache — measured 459s quiet and
  # blew through 900s under contention/eviction (2026-07-23). The ceiling is
  # therefore decided AFTER the build phase, when coldness is knowable: core.a
  # rebuilt this gate (the k2 identity changed => every key is new) or a
  # near-empty bincache => 1800s; otherwise 900s. A genuine per-test hang is
  # still bounded by the harness's 120s compile / 30s run deadlines — this is
  # only the coarse per-shard backstop, and the CANARY is the fail-fast layer,
  # so a generous cold ceiling no longer delays broken-tree detection. A
  # caller's explicit BEZEL_SHARD_TIMEOUT always wins.
  SHARD_ENV=(env BEZEL_AOT_FORCE_SANITIZE=address)   # see #3012 note above
  SHARD_NOTE="asan-instrumented"
  SHARD_LABEL="crash/undercount + generated-code ASan"
  # 2026-07-17: 16 concurrent ASan shards OOM-kill workers under the dev_heavy
  # memory-capped slice (~24G soft / 28G hard) — each ASan compile+exec peaks
  # ~2.5-3G RSS, so 16x exceeds the budget once the box is under pressure and a
  # worker gets OOM-killed => a spurious "shard crash / undercount" FAIL (with a
  # DIFFERENT random set of tests each run). ASan is memory-bound, not CPU-bound,
  # and fewer shards measured FASTER here (8 shards ~270-320s vs 16 shards
  # ~360-965s). So the ASan pass DEFAULTS to a memory-safe shard count (cap 8,
  # tuned to the ~24-28G slice); an explicit GATE_SHARDS overrides it (raise on a
  # larger-memory host, lower on a smaller one). The plain (non-ASan) pass is not
  # memory-bound and keeps run_parallel's nproc default.
  if [ -z "${GATE_SHARDS:-}" ]; then
    _ncpu=$(nproc 2>/dev/null || echo 8)
    GATE_SHARDS=$(( _ncpu < 8 ? _ncpu : 8 ))
  fi
fi

# ---------------------------------------------------------------------------
# Per-phase timing instrumentation — PURE OBSERVATION, never affects gating.
# Each phase below is bracketed with a monotonic-clock start/stop: PHASE_T0 is
# set just before the phase's heavy command, rec_ran/rec_skip just after. At
# exit an aligned human summary is printed and one JSON object is appended to
#   /var/tmp/bezel-scratch/gate-timings/gate-timings.jsonl
# All of it is best-effort: a timing/logging hiccup must never change the gate's
# exit code or the pass/fail output its consumers rely on. (gate_and_push.sh
# keys off gate.sh's EXIT CODE only, not stdout — so the summary block printed
# after "GATE PASS"/"GATE FAIL" is safe to append.)
# ---------------------------------------------------------------------------
now() { awk '{print $1; exit}' /proc/uptime 2>/dev/null || date +%s; }  # monotonic secs since boot
TLOGDIR=/var/tmp/bezel-scratch/gate-timings
TLOG="$TLOGDIR/gate-timings.jsonl"
GATE_T0=$(now)
# Canonical live-status file (#2100 follow-up): one place ANY watcher polls —
# "running" written here, the verdict written by gate_finish on EVERY exit.
# Exists because detached gates (survivors of background-task shedding) emit
# no completion signal of their own; best-effort, never affects gating.
# Honor an inherited GATE_STATUS_FILE so a caller launching a DETACHED gate can
# point the verdict at a PER-RUN path and poll it (the shared default is a single
# file every gate overwrites — useless when gates run back-to-back, and a stale
# reader would see another gate's result). A completion waiter should still key
# on the `head` field == its own gate's commit (or watch the LOG line directly).
GATE_STATUS_FILE="${GATE_STATUS_FILE:-$S/gate-status.json}"
printf '{"status":"running","mode":"%s","head":"%s","pid":%d,"started":"%s"}\n' \
  "$MODE" "$(git rev-parse --short HEAD 2>/dev/null || echo '?')" "$$" \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo '')" \
  > "$GATE_STATUS_FILE" 2>/dev/null || true
GATE_TS=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo "")
GATE_EPOCH=$(date +%s 2>/dev/null || echo 0)
GATE_HOST=$(hostname 2>/dev/null || echo "?")
GATE_HEAD=$(git rev-parse HEAD 2>/dev/null || echo "")
GATE_HEAD_SHORT=$(git rev-parse --short HEAD 2>/dev/null || echo "")
GATE_DIRTY=false; [ -n "$(git status --porcelain 2>/dev/null)" ] && GATE_DIRTY=true
FAILED_PHASE=""
# Context harvested during the run (defaults keep the JSON well-formed regardless).
# serial_* stay 0 (the serial pass was removed in #1861) — kept in the JSON so
# schema:1 consumers keep parsing; asan_* now come from the sharded pass.
SHARD_COUNT=0; SHARD_REPORTED_S=0; SHARD_TIMES=""; ARB_SUSPECTS=0
SERIAL_ATTEMPTS=0; SERIAL_TOTAL=0; ASAN_SHARDS=0; ASAN_ERRS=0
COV_GTESTS=0; COV_LITTESTS=0
declare -a PH_NAME PH_DUR PH_RC PH_STATUS PH_NOTE
PHASE_T0=$(now)
rec_ran()  { PH_NAME+=("$1"); PH_DUR+=("$(awk "BEGIN{printf \"%.2f\", $(now)-$PHASE_T0}")"); PH_RC+=("$2"); PH_STATUS+=("ran"); PH_NOTE+=("${3:-}"); }
rec_skip() { PH_NAME+=("$1"); PH_DUR+=("0.00"); PH_RC+=("0"); PH_STATUS+=("skipped"); PH_NOTE+=("${2:-}"); }

emit_json() { # rc result total_s -> append one JSON object; best-effort
  local rc="$1" result="$2" total="$3"
  mkdir -p "$TLOGDIR" 2>/dev/null || return 0
  local phases="" sep="" i n=${#PH_NAME[@]}
  for (( i=0; i<n; i++ )); do
    phases="${phases}${sep}{\"name\":\"${PH_NAME[$i]}\",\"s\":${PH_DUR[$i]},\"rc\":${PH_RC[$i]},\"status\":\"${PH_STATUS[$i]}\",\"note\":\"${PH_NOTE[$i]}\"}"
    sep=","
  done
  local fp="null"; [ -n "$FAILED_PHASE" ] && fp="\"$FAILED_PHASE\""
  # fix #3010: AN UNMEASURED NUMBER EMITS null, NEVER 0.
  # `"gtests":${COV_GTESTS:-0}` is precisely how 26 records came to say
  # result=pass with gtests=0: a harvest regex that failed and a suite that
  # genuinely ran nothing were byte-identical in the log. `null` means "not
  # measured"; `0` means "measured as zero". Conflating them destroyed the only
  # evidence that would have distinguished them, for 24 days.
  local j_gtests="null"; [ -n "${COV_GTESTS:-}" ] && [ "${COV_GTESTS:-0}" -gt 0 ] 2>/dev/null && j_gtests="$COV_GTESTS"
  local j_lit="null";    [ -n "${COV_LITTESTS:-}" ] && [ "${COV_LITTESTS:-0}" -gt 0 ] 2>/dev/null && j_lit="$COV_LITTESTS"
  local j_exp="null";    [ "${GATE_EXPECTED_GTESTS:-0}" -gt 0 ] 2>/dev/null && j_exp="$GATE_EXPECTED_GTESTS"
  local j_cachen="null"; [ -n "${cache_n:-}" ] && j_cachen="$cache_n"
  local j_sto="null";    [ -n "${sto:-}" ] && j_sto="$sto"
  local j_cold="null";   [ -n "${GATE_COLD:-}" ] && { [ "$GATE_COLD" -eq 1 ] && j_cold=true || j_cold=false; }
  local j_floor="null";  [ -n "${GATE_FLOOR_OK:-}" ] && { [ "$GATE_FLOOR_OK" -eq 1 ] && j_floor=true || j_floor=false; }
  local j_aot="null"
  [ -n "${AOT_HITS:-}${AOT_MISSES:-}" ] && j_aot="{\"hits\":${AOT_HITS:-null},\"misses\":${AOT_MISSES:-null},\"store_failures\":${AOT_STORE_FAILS:-null}}"
  # merge the two flat maps written at different points in the run (see the
  # ordering note at the harvest). Union of names, so a binary that was expected
  # but never reported appears with ran=0 — which is exactly the signature R2
  # looks for ("a whole area contributed nothing").
  local j_bins="null" _m=""
  if [ -n "${GATE_BIN_EXPECTED:-}${GATE_BIN_RAN:-}" ]; then
    _m=$(# NB: `exp` and `ran` are gawk builtins — using them as -v names is a
      # fatal awk error, which would have made this merge emit nothing at all.
      awk -v emap="${GATE_BIN_EXPECTED:-}" -v rmap="${GATE_BIN_RAN:-}" '
      BEGIN{
        n=split(emap,E," "); for(i=1;i<=n;i++){ split(E[i],a,":"); if(a[1]!="")e[a[1]]=a[2] }
        n=split(rmap,R," "); for(i=1;i<=n;i++){ split(R[i],a,":"); if(a[1]!="")r[a[1]]=a[2] }
        for(k in e) seen[k]=1; for(k in r) seen[k]=1
        out=""; sep=""
        for(k in seen){
          ev=(k in e)?e[k]:"null"; rv=(k in r)?r[k]:0
          out=out sep "\"" k "\":{\"ran\":" rv ",\"expected\":" ev "}"; sep=","
        }
        print out
      }')
    [ -n "$_m" ] && j_bins="{$_m}"
  fi
  printf '{"schema":2,"ts":"%s","epoch":%s,"host":"%s","mode":"%s","head":"%s","head_short":"%s","dirty":%s,"result":"%s","failed_phase":%s,"total_s":%s,"gtests":%s,"littests":%s,"expected_gtests":%s,"coverage_floor_enforced":%s,"cold":%s,"cache_n":%s,"shard_timeout_s":%s,"aot":%s,"binaries":%s,"shard_count":%s,"shard_reported_s":%s,"arbitration_suspects":%s,"serial_attempts":%s,"serial_total":%s,"asan_shards_reported":%s,"asan_errors":%s,"shard_times":[%s],"phases":[%s]}\n' \
    "$GATE_TS" "${GATE_EPOCH:-0}" "$GATE_HOST" "$MODE" "$GATE_HEAD" "$GATE_HEAD_SHORT" "$GATE_DIRTY" \
    "$result" "$fp" "$total" "$j_gtests" "$j_lit" "$j_exp" "$j_floor" "$j_cold" "$j_cachen" "$j_sto" \
    "$j_aot" "$j_bins" "${SHARD_COUNT:-0}" "${SHARD_REPORTED_S:-0}" \
    "${ARB_SUSPECTS:-0}" "${SERIAL_ATTEMPTS:-0}" "${SERIAL_TOTAL:-0}" "${ASAN_SHARDS:-0}" "${ASAN_ERRS:-0}" \
    "$SHARD_TIMES" "$phases" >> "$TLOG" 2>/dev/null || return 0
}

gate_finish() { # rc -> print aligned summary + append JSON; must NOT change exit status
  local rc="$1"
  local total; total=$(awk "BEGIN{printf \"%.2f\", $(now)-$GATE_T0}")
  local result="pass"; [ "$rc" -ne 0 ] && result="fail"
  {
    echo "== GATE TIMING SUMMARY (mode=${MODE}, head=${GATE_HEAD_SHORT:-?}, dirty=${GATE_DIRTY}, result=${result}) =="
    printf '   %-18s %10s  %7s   %s\n' "phase" "time" "%total" "status"
    local i n=${#PH_NAME[@]} pct st
    for (( i=0; i<n; i++ )); do
      pct=$(awk "BEGIN{t=$total; d=${PH_DUR[$i]}; printf \"%.1f\", (t>0)?100*d/t:0}")
      if [ "${PH_STATUS[$i]}" = "skipped" ]; then st="skipped"; else st="rc=${PH_RC[$i]}"; fi
      [ -n "${PH_NOTE[$i]}" ] && st="$st (${PH_NOTE[$i]})"
      printf '   %-18s %9ss  %6s%%   %s\n' "${PH_NAME[$i]}" "${PH_DUR[$i]}" "$pct" "$st"
    done
    printf '   %-18s %9ss  %6s%%\n' "TOTAL" "$total" "100.0"
    echo "   machine log: $TLOG"
  } || true
  emit_json "$rc" "$result" "$total" 2>/dev/null || true
  # fix #3010: READ THE TELEMETRY BACK. Writing it was never the gap — the file
  # held 465 records over 24 days and had zero readers, with 55 `pass` verdicts
  # contradicted by their own coverage field sitting in it in plain numbers.
  # Advisory HERE (gate.sh must not change its own exit code from a reporting
  # step); gate_and_push.sh treats the same check as BLOCKING before a push,
  # which is where it can actually stop something.
  if [ -x scripts/gate_trend.sh ]; then
    local _tout; _tout=$(scripts/gate_trend.sh --last 2>&1) || true
    # A checker that silently no-ops is #3000 applied to the fix for #3000, so
    # the ABSENCE of the scan line is itself reported.
    case "$_tout" in
      *"records scanned"*) printf '%s\n' "$_tout" | sed 's/^/   trend| /' ;;
      *) echo "   trend| WARNING: gate_trend.sh produced no scan line — the check did not run" ;;
    esac
  fi
  # Verdict to the canonical status file (see GATE_STATUS_FILE above).
  printf '{"status":"%s","rc":%d,"mode":"%s","head":"%s","total_s":%s,"failed_phase":%s,"finished":"%s"}\n' \
    "$result" "$rc" "$MODE" "$GATE_HEAD_SHORT" "$total" \
    "$([ -n "$FAILED_PHASE" ] && printf '"%s"' "$FAILED_PHASE" || echo null)" \
    "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo '')" \
    > "${GATE_STATUS_FILE:-$S/gate-status.json}" 2>/dev/null || true
  # Phase 3 (#3002): write the AUTHORITATIVE verdict file — ONE atomically-
  # written gate-verdict.json that Phase 3c consumers will read instead of
  # $?/banner greps. ADDITIVE: no consumer reads it yet (observation-only
  # prove-out); the existing $?/banner/status-file outputs above are unchanged.
  # Best-effort `|| true`: a verdict-write hiccup must never change the exit code.
  gate_write_verdict "$rc" "$total"
}

fail() { FAILED_PHASE="$1"; echo "== GATE FAIL: $1 =="; exit 1; }
# Emit summary+JSON on EVERY exit (pass or fail), preserving the exit code exactly.
trap 'rc=$?; gate_finish "$rc" || true; exit $rc' EXIT

# Flake arbitration over a sharded-run log — shared by the canary tier and the
# authoritative pass (#2100; semantics unchanged from the original inline
# block, #1861). Implemented by gate_arbitrate() below, which delegates the
# re-run + verdict to the gate.arbitration Python module (#3010); the contract
# and rationale documented here are the invariants that module preserves.
# Collect ALL suspects: shard CRASHES (died on: a program killed
# its host) AND assertion FAILs. BOTH can be flaky under sharding — a crash, or
# a FAIL from test-pollution (a polluter leaves global state; the round-robin
# lands it with a snapshot/leak test). ARBITRATE uniformly: re-run each
# suspect's SUITE alone up to 5x (gate_arbitrate's --asan keeps ASan forced in
# ASan modes so an ASan-only failure reproduces rather than being falsely cleared
# as flaky). A
# DETERMINISTIC failure fails all 5; a pollution/timing flake passes at least
# once — one run isn't enough, it shares the flake rate. Sets ARB_LAST_SUSPECTS
# for the caller's phase note. Returns:
#   0  every sampled suspect passed alone within 5 tries (sharding flake) — continue
#   1  a sampled suspect fails alone all 5 tries (real regression)
#   2  nonzero exit but NO crash/FAIL marker in the log (undercount/timeout)
# A large suspect list is arbitrated via a CAPPED SAMPLE (<= ARB_SAMPLE),
# never fail-fast on COUNT: a shard killed at the timeout ceiling (cold AOT
# cache / contention) emits a "died on:" victim for every test in flight, so a
# TIMEOUT lists MANY suspects that all pass alone — indistinguishable by count
# from a real cross-cutting regression. Sampling bounds the re-run cost while
# still surfacing a genuine widespread regression (some sampled suspect fails
# all 5). Replaces an earlier ">8 => fail-fast real-regression" that mis-reported
# cold-cache timeouts as mass regressions (the #2205 mis-revert, 2026-07-25).
# The per-area test binaries, from the CMake-generated manifest
# (tests/CMakeLists.txt). Every runner reads this same file, so the set of
# binaries the gate accounts for is exactly the set CMake built.
test_binaries() { cut -f2 build/tests/test_binaries.txt 2>/dev/null; }

# Phase 4 (#3010): arbitration is now the gate.arbitration Python module. The
# module's arbitrate() re-runs each suspect's suite across the per-area binaries
# (its own arb_run_suite — porting subsumes the Phase-2c `^[0-9]+ passed` grep
# that lived in the old arb_run_suite) and returns the 0/1/2 verdict contract
# above. ARB_ENV's ASan-forcing (#2100 invariant 5) is the CLI's --asan flag.
#
# DUAL-RUN (cheap): the module's suspect COUNT is asserted to equal the OLD bash
# greps' count — a live proof of the suspect EXTRACTION (the gate-E false-green
# class, #2100 — [FAIL]-never-sampled) on every gate that reaches arbitration,
# without doubling the re-run cost. The full re-run stays Python-authoritative,
# proven by the unit tests (the 5 invariants pinned in test_arbitration.py); re-
# running every suspect twice is impractical and the suspect SELECTION is the
# part most likely to harbour a subtle port bug. The bash greps below ARE the
# old arbitrate's suspect extraction, inlined as the assertion.
gate_arbitrate() { # $1 = sharded-run log  →  returns 0|1|2; sets ARB_LAST_SUSPECTS
  local log="$1" _asan="" _out _verdict _crashed _failed _bash_suspects
  [ "$ASAN" -eq 1 ] && _asan="--asan"
  _out=$(PYTHONPATH=scripts python3 -m gate.arbitration arbitrate \
    --log "$log" --sample-cap "${ARB_SAMPLE:-8}" $_asan 2>&1); _verdict=$?
  ARB_LAST_SUSPECTS=$(printf '%s\n' "$_out" | sed -n 's/^@arb:suspects=//p' | head -1); ARB_LAST_SUSPECTS=${ARB_LAST_SUSPECTS:-0}
  # Echo the FLAKY/REAL/cap detail lines (the @arb: line is orchestrator-only).
  printf '%s\n' "$_out" | grep -v '^@arb:'
  # --- dual-run assertion (TEMPORARY: remove after the landing gate proves it) ---
  # The OLD bash greps' unique-suspect count: crashed + failed, minus blank,
  # minus <none> (a shard killed at startup is not an arbitrable suspect).
  _crashed=$(grep 'died on:' "$log" | sed -E 's/.*died on: //')
  _failed=$(grep -E '^\[ FAIL' "$log" | sed -E 's/^\[ FAIL \] //' | awk '{print $1}')
  _bash_suspects=$(printf '%s\n%s\n' "$_crashed" "$_failed" | grep -v '^$' | grep -vx '<none>' | sort -u | grep -c . || true)
  if [ "$_bash_suspects" -ne "$ARB_LAST_SUSPECTS" ]; then
    echo "## GATE FAILURE: arbitration PYTHON/BASH DRIFT — suspects(bash=$_bash_suspects py=$ARB_LAST_SUSPECTS)" >&2
    echo "##   gate.arbitration parsed a different suspect set than the proven bash;" >&2
    echo "##   a re-run over a divergent suspect set is untrustworthy (#2100)." >&2
    FAILED_PHASE="arbitration python/bash drift: suspects(bash=$_bash_suspects py=$ARB_LAST_SUSPECTS)"
    exit 3
  fi
  # --- end dual-run assertion ---
  return "$_verdict"
}

echo "== gate[1/6]: reconfigure (glob any new test files) =="
# Snapshot the compiler-archive mtime BEFORE any build work: comparing after
# the build phase tells us whether the k2 cache identity changed this gate
# (core.a rebuilt => cold ASan pass guaranteed) — see the ceiling note above.
CORE_MTIME_PRE=$(stat -c %Y build/libbezel_core.a 2>/dev/null || echo 0)
PHASE_T0=$(now)
"$DH" build cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$S/gate_reconf.log" 2>&1; prc=$?
rec_ran configure "$prc"
[ "$prc" -ne 0 ] && { tail -15 "$S/gate_reconf.log"; fail "configure"; }

# `bezel_tests` is the AGGREGATE target over the per-area binaries
# (tests/CMakeLists.txt) — the target name is unchanged, so this phase is too.
echo "== gate[2/6]: build bezel_tests (all per-area binaries) =="
PHASE_T0=$(now)
"$DH" build cmake --build build --target bezel_tests -j16 >"$S/gate_build.log" 2>&1; prc=$?
rec_ran build "$prc"
[ "$prc" -ne 0 ] && { grep -iE 'error:|FAILED:' "$S/gate_build.log" | head -20; fail "build"; }
# A missing/empty manifest means the gate has no idea what to run — refuse
# rather than silently sharding a subset of the suite.
[ -s build/tests/test_binaries.txt ] || fail "build produced no test-binary manifest (build/tests/test_binaries.txt)"
while IFS= read -r _b; do
  [ -n "$_b" ] || continue
  [ -x "$_b" ] || fail "build reported success but test binary $_b is missing"
  echo "   binary: $(basename "$_b")  $(ls -la --time-style=+%H:%M:%S "$_b" | awk '{print $6}')"
done < <(test_binaries)

# Decide the per-shard ceiling now that coldness is knowable (#2100; see the
# SHARD_ENV note above). Cold predicate: core.a rebuilt this gate (k2 identity
# changed => every cache key new) OR a near-empty bincache (fresh box or
# eviction — a warm full-suite state holds ~11k+ entries; the find is cheap and
# uncapped). Explicit BEZEL_SHARD_TIMEOUT wins unconditionally.
#
# fix #2918: this block used to sit inside `if [ "$ASAN" -eq 1 ]`, so BOTH
# oracles were gated on it but only ONE got it. The plain pass fell through to
# tests/run_parallel.sh's hardcoded `${BEZEL_SHARD_TIMEOUT:-300}` — 300s per
# shard, cold or warm — which is the mechanical reason every gate on 2026-08-06
# needed 2+ attempts on the PLAIN oracle. The workaround was an operator-
# remembered env pair (BEZEL_AOT_CACHE=0 + BEZEL_SHARD_TIMEOUT=1800); it bought
# 4 attempt-1 greens out of 6 oracles versus roughly 0 of 8 before — a strong
# improvement, NOT a cure, and one that lived only in a human's memory of which
# variables to export. Same shape as #2917/#2100: a gate lesson that is
# remembered rather than structural.
#
# ⚠ THE IDENTITY MARKER MUST BE KEYED PER ORACLE, and that is not cosmetic. The
# AOT cache key hashes the sanitize bit (aot_support.hpp:1121-1122, `s += "sa=";
# s += opts.sanitizeAddress ? '1' : '0';`), so plain and ASan entries are
# DISJOINT POPULATIONS IN ONE STORE. Before this change only the ASan pass wrote
# the marker, so nothing was contaminated; the moment the plain pass also writes
# it, a gate_full_cycle run would go: plain completes -> records the new identity
# -> the ASan pass that follows reads "match" -> predicts WARM -> takes 900s ->
# while holding ZERO ASan entries under that identity. That is exactly the
# mis-prediction this marker exists to prevent (2026-07-29: gates C and D given
# 900s for a ~2400s cold run, killed at it having completed ~37% of the suite).
# Hence .gate-identity vs .gate-identity-asan.
#
# For the same reason `cache_n` below is ORACLE-BLIND: entry filenames are
# content-addressed, so the `sa=` bit is not inspectable and a store holding 11k
# plain entries and 0 ASan entries reads "warm" to both. The count can therefore
# only ever err toward warm; the per-oracle identity marker is the load-bearing
# signal and is what actually forces the cold verdict.
#
# ⚠ `find -L` IS LOAD-BEARING (#3000). The default cache root is a SYMLINK on
# this box (/var/tmp/bezel-scratch/aot-bincache -> /mnt/fast/aot-bincache), and
# `find` does not traverse a symlinked START POINT without -L. Without it this
# read returned 0 unconditionally — against a store holding 102,299 entries —
# so `cache_n < 2000` always fired, the gate always predicted COLD, the warm
# ceilings were unreachable dead code, and the log printed "bincache entries=0"
# into the one place a human arbitrating a red gate goes looking for evidence.
# That is the third variant of the category-defect taxonomy: a correct predicate
# fed by a source that never held the population. The predicate was fine; the
# `find` never returned the population. Do not drop the -L.
core_post=$(stat -c %Y build/libbezel_core.a 2>/dev/null || echo -1)
GATE_CACHE_ROOT="${BEZEL_AOT_CACHE_DIR:-/var/tmp/bezel-scratch/aot-bincache}"
# Phase 5 (#3010): the k2 identity, the cold/warm prediction, and the per-oracle
# marker are now the gate.identity / gate.cold Python modules. Python is
# AUTHORITATIVE; the bash recompute in the dual-run assertion below is retained
# for the landing-gate equivalence proof and removed once proven (per #3010's
# add-then-prove-then-switch discipline). The invariants the modules encode are
# UNCHANGED — #2100 (the 3-signal cold predicate), #2918 (the per-oracle marker),
# #3000 (find -L) — see the module docstrings and the block comments above.
oracle="plain"; [ "$ASAN" -eq 1 ] && oracle="asan"
GATE_CACHE_ID=$(PYTHONPATH=scripts python3 -m gate.identity compute \
  --core build/libbezel_core.a --runtime build/libbezel_runtime.a)
# per #2918: the marker filename MUST be per-oracle (plain/ASan are disjoint
# populations in one store); marker_filename encodes that branch.
GATE_CACHE_ID_FILE=$(PYTHONPATH=scripts python3 -m gate.identity marker-filename \
  --cache-root "$GATE_CACHE_ROOT" --oracle "$oracle")
_recorded_id=$(PYTHONPATH=scripts python3 -m gate.identity read-marker --path "$GATE_CACHE_ID_FILE")
identity_changed=0; [ "$core_post" != "$CORE_MTIME_PRE" ] && identity_changed=1
gate_id_note="match"; marker_changed=0
if [ -n "$GATE_CACHE_ID" ]; then
  if [ "$GATE_CACHE_ID" != "$_recorded_id" ]; then
    marker_changed=1; gate_id_note="CHANGED (cache is cold for this tree)"
  fi
else
  gate_id_note="unavailable"
fi
# cold.evaluate: count the cache (find -L, #3000), apply the 3-signal predicate
# (#2100), and pick the ceiling (#2918: cold 1800 / warm 900 for BOTH oracles —
# the warm 900s is NOT the old 300s fallback; see the ceiling comment above).
# Prints shell-sourceable cache_n / cold / ceiling. Fail-safe defaults below
# keep the gate cold if the module is unavailable (python3 is already a hard
# gate dependency via verdict/summary/trend; this just keeps `set -u` safe).
cache_n=0 cold=1 ceiling=1800
eval "$(PYTHONPATH=scripts python3 -m gate.cold evaluate \
  --cache-root "$GATE_CACHE_ROOT" \
  --identity-changed "$identity_changed" --marker-changed "$marker_changed" 2>/dev/null)"
GATE_COLD=$cold
# A caller's explicit BEZEL_SHARD_TIMEOUT always wins (see GATE_EXPLICIT_STO).
sto="$GATE_EXPLICIT_STO"; [ -z "$sto" ] && sto=$ceiling
# --- dual-run assertion (TEMPORARY: remove after the landing gate proves it) ---
# Recompute the cold/identity signals the OLD bash way and assert the Python
# modules agree. Fatal on drift: a disagreement is a real port defect, and the
# gate must not proceed on an unproven value (#3010 — the gate stays
# trustworthy; the old bash is authority until the Python is proven, and if they
# DISAGREE neither is trusted — exit 3 = retry, never a silent pass).
_bash_cache_n=$(find -L "$GATE_CACHE_ROOT" -type f 2>/dev/null | wc -l)
_bash_id=$(cat build/libbezel_core.a build/libbezel_runtime.a 2>/dev/null | sha256sum 2>/dev/null | cut -d' ' -f1)
_bash_recorded=$(cat "$GATE_CACHE_ID_FILE" 2>/dev/null || echo none)
_bash_cold=0
{ [ "$core_post" != "$CORE_MTIME_PRE" ] || [ "$_bash_cache_n" -lt 2000 ] || { [ -n "$_bash_id" ] && [ "$_bash_id" != "$_bash_recorded" ]; }; } && _bash_cold=1
_p5_drift=""
[ "$_bash_cache_n" -ne "$cache_n" ] && _p5_drift="${_p5_drift}cache_n(bash=$_bash_cache_n py=$cache_n) "
[ "$_bash_cold" -ne "$GATE_COLD" ] && _p5_drift="${_p5_drift}cold(bash=$_bash_cold py=$GATE_COLD) "
# The identity hashes agree whenever BOTH archives exist (the build phase
# guarantees this); they diverge only in the both-missing case, where bash's
# `sha256sum` of empty input yields e3b0c44… while the module fails CLOSED ("").
# Assert the digest only when the module produced one — i.e. both archives read.
if [ -n "$GATE_CACHE_ID" ] && [ "$_bash_id" != "$GATE_CACHE_ID" ]; then
  _p5_drift="${_p5_drift}identity(bash=$_bash_id py=$GATE_CACHE_ID) "
fi
if [ -n "$_p5_drift" ]; then
  echo "## GATE FAILURE: Phase-5 PYTHON/BASH DRIFT — $_p5_drift" >&2
  echo "##   gate.identity/gate.cold disagree with the proven bash; refusing to" >&2
  echo "##   proceed on an unproven cold/identity value (retry once fixed)." >&2
  FAILED_PHASE="phase-5 python/bash drift: $_p5_drift"
  exit 3
fi
# --- end dual-run assertion ---
SHARD_ENV+=(BEZEL_SHARD_TIMEOUT="$sto")
core_note="unchanged"; [ "$core_post" != "$CORE_MTIME_PRE" ] && core_note="rebuilt"
cold_note="warm-predicted"; [ "$GATE_COLD" -eq 1 ] && cold_note="cold-predicted"
_sto_label="plain"; [ "$ASAN" -eq 1 ] && _sto_label="ASan"
echo "   $_sto_label shard ceiling: ${sto}s ($cold_note: core.a $core_note, bincache entries=$cache_n, identity $gate_id_note)"

# lit + FileCheck diagnostic track (tests/lit/): drives the REAL bezelc over .bzl
# files and matches diagnostics with FileCheck. Fast (~0.1s/test, no JIT) and
# per-process, so it validates the accept/reject + diagnostic-text/location
# surface without the sharding-pollution or wrong-harness (#1682) failure modes
# the gtest suite is exposed to. Skipped cleanly if lit/FileCheck/not were absent
# at configure (the site config is only generated when all three are found).
echo "== gate[3/6]: lit diagnostic track (real bezelc + FileCheck, per-process) =="
if [ -f build/tests/lit/lit.site.cfg.py ]; then
  PHASE_T0=$(now)
  "$DH" build cmake --build build --target bezelc check-lit -j16 >"$S/gate_lit.log" 2>&1; lit_rc=$?
  rec_ran lit "$lit_rc"
  grep -E 'Total Discovered|Passed:|Failed:|Unresolved|Unsupported' "$S/gate_lit.log" | tail -4 || true
  [ "$lit_rc" -ne 0 ] && { echo "   -- lit failures --"; grep -E 'FAIL:|error:|expected string not found' "$S/gate_lit.log" | head -20; tail -8 "$S/gate_lit.log"; fail "lit diagnostic track"; }
else
  echo "   (check-lit target not configured — lit/FileCheck/not absent; skipping)"
  rec_skip lit "not configured"
fi

# CANARY tier (#2100): the diff-selected subset, run BEFORE the authoritative
# pass. FAIL-FAST ONLY — a red canary fails the gate in minutes instead of a
# broken tree grinding every shard to the timeout ceiling; a green canary
# proves nothing beyond its subset and the full pass below remains the sole
# acceptance authority. Inherits the gate's ASan mode (SHARD_ENV) and shares
# arbitrate() with the full pass. Selector/infra problems SKIP (fail-open).
# See the CANARY block in the header for the design and knobs.
echo "== gate[4/6]: canary — diff-selected subset (fail-fast only; never acceptance) =="
CANARY_LIST="$S/gate_canary.list"
canary_skip() { echo "   (canary skipped: $1)"; rec_skip canary "$1"; }
if [ "${GATE_CANARY:-1}" = "0" ]; then
  canary_skip "GATE_CANARY=0"
else
  PHASE_T0=$(now)
  cbase="${GATE_CANARY_BASE:-}"
  [ -z "$cbase" ] && cbase="$(git merge-base HEAD origin/main 2>/dev/null || true)"
  if [ -z "$cbase" ]; then
    canary_skip "no base (origin/main unavailable and no GATE_CANARY_BASE)"
  elif ! python3 scripts/select_tests.py --base "$cbase" -o "$CANARY_LIST" >"$S/gate_canary_select.log" 2>&1; then
    tail -5 "$S/gate_canary_select.log"
    canary_skip "selector failed (fail-open; see $S/gate_canary_select.log)"
  else
    nsel=$(grep -c . "$CANARY_LIST" 2>/dev/null); nsel=${nsel:-0}
    nuniv=$(grep -oE 'universe: [0-9]+' "$S/gate_canary_select.log" | grep -oE '[0-9]+' | tail -1); nuniv=${nuniv:-0}
    maxpct="${GATE_CANARY_MAX_PCT:-50}"
    if [ "$nsel" -eq 0 ]; then
      canary_skip "empty selection (no mapped src/test diff vs base)"
    elif [ "$nuniv" -gt 0 ] && [ $(( 100 * nsel / nuniv )) -ge "$maxpct" ]; then
      canary_skip "selection $nsel/$nuniv >= ${maxpct}% of suite — full pass covers it"
    else
      echo "   canary: $nsel of $nuniv tests selected (base $(git rev-parse --short "$cbase" 2>/dev/null || echo "$cbase"))"
      grep -E '^signals:' "$S/gate_canary_select.log" | head -1 || true
      "$DH" suite "${SHARD_ENV[@]}" env BEZEL_TEST_LIST="$CANARY_LIST" tests/run_parallel.sh ${GATE_SHARDS:-} >"$S/gate_canary.log" 2>&1; crc=$?
      rec_ran canary "$crc" "$nsel/$nuniv selected${SHARD_NOTE:+, $SHARD_NOTE}"
      grep -E 'TOTAL:|WARNING: only' "$S/gate_canary.log" || true
      # A fired generated-code sanitizer report in the canary is DETERMINISTIC
      # evidence, exactly as in the full pass — fail hard, no arbitration.
      if [ "$ASAN" -eq 1 ]; then
        c_asan=$(grep -cE 'ERROR: (Address|Leak)Sanitizer|aot-asan #1822\] sanitizer fired' "$S/gate_canary.log" 2>/dev/null)
        if [ "${c_asan:-0}" -gt 0 ]; then
          echo "   -- generated-code ASan fired in the canary ($c_asan report marker(s)) --"
          grep -m3 -A18 -E 'ERROR: (Address|Leak)Sanitizer' "$S/gate_canary.log" | head -60
          fail "canary ASan — memory error in a diff-selected test program (see $S/gate_canary.log)"
        fi
      fi
      if [ "$crc" -ne 0 ]; then
        PHASE_T0=$(now)  # canary arbitration is its own (exceptional) phase
        gate_arbitrate "$S/gate_canary.log"; carb=$?
        case "$carb" in
          0) rec_ran canary_arbitration "0" "$ARB_LAST_SUSPECTS suspects, all flaky"
             echo "   all canary suspects flaky (pass alone within 5 tries) -> continue to the full pass";;
          2) # No arbitrable crash/FAIL marker: an INFRA shape — all-phantom
             # list (subset_empty), a shard dead before any test, an
             # undercount, a launch hiccup. Per the canary contract this is
             # FAIL-OPEN: warn loudly and continue — the authoritative full
             # pass follows unconditionally and catches anything real. (A
             # genuine failing/hanging selected test produces a died-on/FAIL
             # marker and takes the arbitrated paths instead.)
             rec_ran canary_arbitration "0" "no crash/FAIL marker — infra shape, fail-open"
             grep -E '## GATE FAILURE|SUBSET' "$S/gate_canary.log" | head -3 || true
             echo "   canary: nonzero exit with NO crash/FAIL marker (infra/selector shape) -> FAIL-OPEN to the full pass (see $S/gate_canary.log)";;
          *) rec_ran canary_arbitration "1" "$ARB_LAST_SUSPECTS suspects, real"
             fail "canary (diff-selected subset) — regression caught BEFORE the full pass (see $S/gate_canary.log)";;
        esac
      fi
    fi
  fi
fi

# clang-tidy static-analysis tier (tests/CMakeLists.txt target check-tidy;
# design + config: test-infra migration plan §2 / WS7 —
# /var/tmp/bezel-scratch/impl-run-2026-07-12/aot-plan/ws7-static-analysis.md).
# ADVISORY ONLY: reports findings, does NOT fail the gate — WS7 measured a
# ~70-80% false-positive rate among substantive findings on this ASan-hardened
# codebase (report §2.5/§4); promotion to gate-blocking is a follow-on once the
# real findings (R1-R7) are fixed and the confirmed FPs are NOLINT-annotated.
# Changed-TUs mode only, mirroring check-lit's "skip cleanly if absent" guard:
# a full 62-TU sweep is ~4-5 min at -j16 (WS7 §2.3) — heavier than a clean
# build, so it's a consolidation/CI cost (`cmake --build build --target
# check-tidy`), not a per-merge one. Skips cleanly if clang-tidy /
# run-clang-tidy are absent.
echo "== gate[5/6]: clang-tidy static-analysis (advisory; changed TUs only) =="
if command -v clang-tidy >/dev/null 2>&1 && command -v run-clang-tidy >/dev/null 2>&1; then
  base_ref="$(git merge-base HEAD main 2>/dev/null || true)"
  [ -z "$base_ref" ] && base_ref="HEAD~1"
  PHASE_T0=$(now)
  "$DH" build scripts/check_tidy.sh build --changed "$base_ref" >"$S/gate_tidy.log" 2>&1; tidy_rc=$?
  rec_ran tidy "$tidy_rc" "advisory"
  grep -E 'check-tidy:' "$S/gate_tidy.log" | head -10 || true
  # advisory: intentionally no `fail` call here regardless of findings/exit code
else
  echo "   (clang-tidy or run-clang-tidy not found — skipping; advisory tier disabled)"
  rec_skip tidy "no clang-tidy"
fi

echo "== gate[6/6]: sharded suite ($SHARD_LABEL) — the authoritative oracle =="
PHASE_T0=$(now)
# per #3012 Phase 2c: name a merged run-summary path for the MAIN shard run
# only (not the canary above — its summary would otherwise clobber this one).
# run_parallel writes it; the harvest below reads it instead of grepping the
# printed TOTAL / per-binary lines. $S is /var/tmp/bezel-scratch (line 76).
GATE_RUN_SUMMARY="$S/gate_run_summary.json"
SHARD_ENV+=(BEZEL_RUN_SUMMARY="$GATE_RUN_SUMMARY")
"$DH" suite "${SHARD_ENV[@]}" tests/run_parallel.sh ${GATE_SHARDS:-} >"$S/gate_shard.log" 2>&1; rc=$?
# When ASan is on (--full), the shard phase's recorded duration INCLUDES the
# ASan compile+run (SHARD_NOTE marks it), so the ASan cost is captured in the
# phases array here — there is no separate asan phase.
rec_ran shard "$rc" "$SHARD_NOTE"
# Harvest run_parallel's own aggregate wall + shard count + per-shard times (obs only).
SHARD_COUNT=$(grep -oE 'across [0-9]+ shards' "$S/gate_shard.log" | grep -oE '[0-9]+' | tail -1); SHARD_COUNT=${SHARD_COUNT:-0}
SHARD_REPORTED_S=$(grep -oE 'across [0-9]+ shards, [0-9]+s' "$S/gate_shard.log" | grep -oE '[0-9]+s' | tr -d 's' | tail -1); SHARD_REPORTED_S=${SHARD_REPORTED_S:-0}
# Shard labels are "<group>.<index>" since the per-area split (they used to be
# a bare index); observation only, but keep the harvest working.
SHARD_TIMES=$(grep -oE '^shard [A-Za-z0-9_]+\.[0-9]+: [0-9.]+s$' "$S/gate_shard.log" | sed -E 's/^.*: ([0-9.]+)s$/\1/' | paste -sd, -)
grep -E 'TOTAL:|## GATE FAILURE|died on|--- failing' "$S/gate_shard.log" || true
# ASan on (--full / GATE_FAST_ASAN=1): a fired generated-code sanitizer report is
# DETERMINISTIC evidence of a memory error — fail hard here, independent of the
# flake arbitration (which re-runs a suspect alone and, absent this, could clear
# an ASan-only failure). The harness re-emits any report with the '[aot-asan
# #1822]' banner; also catch the raw sanitizer headline. Skipped entirely in the
# plain default (a clean suite never matches anyway).
if [ "$ASAN" -eq 1 ]; then
  PHASE_T0=$(now)  # shard_asan is only the grep; the ASan compile+run time is
                   # already attributed to the 'shard' phase above (no double-count).
  asan_hits=$(grep -cE 'ERROR: (Address|Leak)Sanitizer|aot-asan #1822\] sanitizer fired' "$S/gate_shard.log" 2>/dev/null)
  ASAN_ERRS=${asan_hits:-0}; ASAN_SHARDS=$SHARD_COUNT
  if [ "${asan_hits:-0}" -gt 0 ]; then
    echo "   -- generated-code ASan fired in the sharded pass ($asan_hits report marker(s)) --"
    grep -m3 -A18 -E 'ERROR: (Address|Leak)Sanitizer' "$S/gate_shard.log" | head -60
    rec_ran shard_asan "1" "$asan_hits ASan report(s)"
    fail "sharded ASan — memory error in a test program (see $S/gate_shard.log)"
  fi
  rec_ran shard_asan "0" "no ASan report"
fi
if [ "$rc" -ne 0 ]; then
  PHASE_T0=$(now)  # the flake-arbitration re-runs are their own (exceptional) phase
  # Shared arbitrate() (see its definition above for the full rationale, #1861/
  # #2100). With serial gone (#1861) THIS is the authoritative re-check: a
  # suspect that passes alone within 5 tries is a sharding flake and the gate
  # continues; a suspect that fails all 5 is a real regression and fails.
  gate_arbitrate "$S/gate_shard.log"; arb=$?
  ARB_SUSPECTS=$ARB_LAST_SUSPECTS
  case "$arb" in
    2) rec_ran shard_arbitration "1" "no crash/FAIL marker"
       fail "sharded suite — nonzero exit, no crash/FAIL marker (see $S/gate_shard.log)";;
    1) rec_ran shard_arbitration "1" "$ARB_SUSPECTS suspects, a real fail"
       fail "sharded suite — a suspect fails alone all 5 tries (real regression)";;
    0) rec_ran shard_arbitration "0" "$ARB_SUSPECTS suspects, all flaky"
       echo "   all sharded suspects flaky (pass alone within 5 tries) -> ACCEPT (shard-authoritative; no serial backstop)";;
  esac
fi

# Combined coverage: the gtest behavior track and the lit diagnostic track are
# separate runners, so neither alone reports the real total. Sum them here (this
# is the one place that ran both). Best-effort — never fails the gate.
#
# per #3012 Phase 2c: prefer the structured run-summary run_parallel wrote
# (BEZEL_RUN_SUMMARY) over grepping the printed TOTAL line — the JSON is the
# authoritative accounting. The grand total (gtests) and the per-binary ran map
# (GATE_BIN_RAN, further below) both consume one harvest. Fall back to the
# legacy grep when the run-summary is absent (python3 missing, or an older
# run_parallel) so the gate stays sound — the grep is the retained fallback
# assertion until N gates prove the JSON path, then it is removed.
GATE_RUN_HARVEST=""; GATE_RUN_PRESENT=0
if [ -n "${GATE_RUN_SUMMARY:-}" ] && command -v python3 >/dev/null 2>&1; then
  GATE_RUN_HARVEST=$(PYTHONPATH=scripts python3 -m gate.summary run-summary harvest "$GATE_RUN_SUMMARY" 2>/dev/null || true)
  if printf '%s\n' "$GATE_RUN_HARVEST" | grep -q '^rp_run_present=1$'; then
    eval "$GATE_RUN_HARVEST"; GATE_RUN_PRESENT=1
  fi
fi
if [ "$GATE_RUN_PRESENT" -eq 1 ]; then
  gtests=$rp_run_gtests
else
  gtests=$(grep -oE 'TOTAL: [0-9]+ passed' "$S/gate_shard.log" 2>/dev/null | grep -oE '[0-9]+' | tail -1)
fi
littests=$(grep -oE 'Passed: *[0-9]+' "$S/gate_lit.log" 2>/dev/null | grep -oE '[0-9]+' | tail -1)
COV_GTESTS=${gtests:-0}; COV_LITTESTS=${littests:-0}
if [ -n "${gtests:-}" ]; then
  echo "== gate: coverage: ${gtests} gtest + ${littests:-0} lit = $(( gtests + ${littests:-0} )) tests passing =="
fi

# fix #3010: harvest what the run ALREADY PRINTED and threw away.
#
# (a) PER-BINARY counts. run_parallel.sh writes one structured per-binary row
#     into the run-summary (Phase 2c; the legacy `  <name> N passed` line is the
#     fallback). Merging it with the registry's per-binary expectation gives the
#     "a whole area contributed zero" signature directly — 16482 - 11326 = 5156
#     is not a coincidence, it is the codegen binary contributing nothing, which
#     no aggregate total can distinguish from a uniformly short run.
# (b) AOT CACHE STATS. Never collected before today (see SHARD_ENV above).
if [ -r "$S/gate_shard.log" ]; then
  # NOTE ON ORDERING (caught in review, and it is the same defect class as
  # everything else fixed today): this harvest runs BEFORE the coverage-floor
  # block that enumerates the registry, so `expected` is not known yet. Recording
  # RAN and EXPECTED into separate flat maps and merging them in emit_json is the
  # only correct order — an earlier draft read `expected` here (always empty) and
  # was then overwritten by the floor block, so the per-binary data would have
  # been silently absent and R2 would never have fired.
  GATE_BIN_RAN=""
  if [ "$GATE_RUN_PRESENT" -eq 1 ] && [ -n "${rp_run_bin_ran:-}" ]; then
    # Phase 2c: the run-summary's per-binary map is already in the
    # "name:passed " format GATE_BIN_RAN uses (and the awk merge at emit_json
    # consumes); adopt it verbatim with the trailing space the merge expects.
    GATE_BIN_RAN="${rp_run_bin_ran} "
  else
    while read -r _bn _bp; do
      [ -n "$_bn" ] || continue
      GATE_BIN_RAN="${GATE_BIN_RAN}${_bn}:${_bp:-0} "
    done < <(grep -aE '^  [a-z_]+ +[0-9]+ passed, [0-9]+ failed' "$S/gate_shard.log" 2>/dev/null \
               | awk '{print $1, $2}')
  fi

  AOT_HITS=$(grep -aoE '\[aot-cache\] hits=[0-9]+' "$S/gate_shard.log" 2>/dev/null \
               | grep -oE '[0-9]+$' | awk '{t+=$1} END{if(NR)print t}')
  AOT_MISSES=$(grep -aoE 'misses=[0-9]+' "$S/gate_shard.log" 2>/dev/null \
               | grep -oE '[0-9]+$' | awk '{t+=$1} END{if(NR)print t}')
  if [ -n "${AOT_HITS:-}${AOT_MISSES:-}" ]; then
    echo "== gate: AOT cache: ${AOT_HITS:-?} hits / ${AOT_MISSES:-?} misses =="
  fi
fi
# Whole-suite-timeout acceptance is LOUD (#2100): when the sharded pass exited
# nonzero and the gate is passing purely because every arbitrated suspect
# cleared, a zero/absent gtest total means the shards were killed at the
# ceiling and this run validated only the suspect suites — the pass is
# legitimate under the shard-authoritative design (the suspects are the only
# evidence of failure, and they cleared), but the coverage claim belongs to
# the previous/next completed pass, and a reader scanning the log must not
# mistake this for a full-suite validation.
if [ "${rc:-0}" -ne 0 ] && [ "${COV_GTESTS:-0}" -eq 0 ]; then
  echo "== gate: WARNING — accepted via flake arbitration with NO completed gtest total (whole-suite timeout shape): only the arbitrated suspect suites were validated in THIS run =="
fi

# fix #3002: COVERAGE FLOOR — count what ran, compare it to WHAT EXISTS.
#
# The warning above fires only on COV_GTESTS == 0, i.e. it recognises TOTAL
# coverage loss and is blind to PARTIAL loss — "total is not zero, therefore the
# run was complete". Measured on 2026-08-07 (f60c1cb, plain attempt 1): the
# sharded pass reported 5156 of 16482 (31%), every crashed shard was a timeout
# victim, flake arbitration cleared them all (a suite killed at the ceiling
# ALWAYS passes when re-run alone — its process was terminated, it never
# failed), and gate.sh printed `== GATE PASS ==` with rc=0 and no warning at
# all. 11,326 tests never ran and the gate said pass.
#
# No refinement of the banner or the warning can fix that, because the only
# thing that distinguishes a complete run from a truncated one is the number
# that SHOULD have run — which the gate never asked for. So ask. The registry
# is authoritative and cheap: BEZEL_TEST_ENUMERATE=1 lists what a process WOULD
# run without running it (~30ms per binary), and test_binaries.txt is a
# configure-time manifest whose partition of tests/**/*.cpp is enforced by two
# FATAL_ERROR invariants — so this sum is exactly the whole suite, by
# construction, and cannot drift as areas are added.
#
# This subsumes the undercount heuristics rather than joining them: they infer
# truncation from symptoms (a crashed shard, a missing summary, a zero total);
# this measures it.
#
# fix #3011: THE FLOOR'S OWN INPUT WAS THE DEFECT IT WAS WRITTEN TO CATCH.
# The first version skipped a manifest row with `[ -x "$bin" ] || continue`, so
# a binary that failed to link, was never built, or was renamed did not fail the
# gate — it silently LOWERED the bar it was supposed to raise. Miss the codegen
# binary and `expected` drops from 16,482 to 5,156, which is exactly the number
# the run that motivated #3002 reported as a pass. The floor would have agreed
# with it.
#
# That is variant 3 of this project's category-defect taxonomy — a correct
# predicate fed by a source collection that never held the population — sitting
# inside the fix for a variant-3 defect. So: count the rows, require every row
# to yield a number, and require the row count to be non-zero. Any gap is a hard
# failure, never a smaller expectation.
# Phase 4 (#3010): the coverage floor is now the gate.coverage Python module.
# The CLI's EXIT CODE is the decision (0 ok / 3 retry — the same exit-3 retry
# contract gate_full_cycle.sh relies on). stdout carries the banner (echoed
# below, excluding the @cov: machine lines) plus machine lines this orchestrator
# harvests for telemetry + the trap's FAILED_PHASE. Python is AUTHORITATIVE; the
# bash re-enumerate in the dual-run assertion below is retained for the
# landing-gate equivalence proof (#3011: a missing/non-executable/empty-
# enumerate binary is a HARD fail that NEVER lowers the bar).
GATE_EXPECTED_GTESTS=0
GATE_FLOOR_OK=1
GATE_BIN_EXPECTED=""
_cov_out=$(PYTHONPATH=scripts python3 -m gate.coverage check \
  --ran "${COV_GTESTS:-0}" --manifest build/tests/test_binaries.txt \
  --per-binary-ran "${GATE_BIN_RAN:-}"); _cov_rc=$?
# Echo the human banner verbatim (the @cov: machine lines are orchestrator-only).
printf '%s\n' "$_cov_out" | grep -v '^@cov:'
# Harvest the machine values emit_json + the trap consume.
GATE_EXPECTED_GTESTS=$(printf '%s\n' "$_cov_out" | sed -n 's/^@cov:expected=//p' | head -1); GATE_EXPECTED_GTESTS=${GATE_EXPECTED_GTESTS:-0}
GATE_FLOOR_OK=$(printf '%s\n' "$_cov_out" | sed -n 's/^@cov:floor_ok=//p' | head -1); GATE_FLOOR_OK=${GATE_FLOOR_OK:-0}
GATE_BIN_EXPECTED=$(printf '%s\n' "$_cov_out" | sed -n 's/^@cov:bin_expected=//p' | head -1)
# --- dual-run assertion (TEMPORARY: remove after the landing gate proves it) ---
# Re-enumerate the registry the OLD bash way and assert the module agrees on the
# decision-critical values: the expected total and the floor usability. Fatal on
# drift. The bash re-enumerate is itself the #3011 authority (a missing binary ⇒
# hard-fail), so a divergence here is exactly the defect this floor catches.
_bash_exp=0; _bash_floor_ok=1; _bash_rows=0
if [ -r build/tests/test_binaries.txt ]; then
  while read -r _g _bin _n; do
    [ -n "$_bin" ] || continue
    _bash_rows=$(( _bash_rows + 1 ))
    if [ ! -x "$_bin" ]; then _bash_floor_ok=0; continue; fi
    _en=$(BEZEL_TEST_ENUMERATE=1 "$_bin" 2>/dev/null | wc -l)
    [ "${_en:-0}" -le 0 ] && { _bash_floor_ok=0; continue; }
    _bash_exp=$(( _bash_exp + _en ))
  done < build/tests/test_binaries.txt
else
  _bash_floor_ok=0
fi
[ "$_bash_rows" -eq 0 ] && _bash_floor_ok=0
_cov_drift=""
[ "$_bash_exp" -ne "$GATE_EXPECTED_GTESTS" ] && _cov_drift="${_cov_drift}expected(bash=$_bash_exp py=$GATE_EXPECTED_GTESTS) "
[ "$_bash_floor_ok" -ne "$GATE_FLOOR_OK" ] && _cov_drift="${_cov_drift}floor_ok(bash=$_bash_floor_ok py=$GATE_FLOOR_OK) "
if [ -n "$_cov_drift" ]; then
  echo "## GATE FAILURE: coverage-floor PYTHON/BASH DRIFT — $_cov_drift" >&2
  echo "##   gate.coverage disagrees with the proven bash; refusing to proceed on" >&2
  echo "##   an unproven floor (#3011 — a missing binary must never lower it)." >&2
  FAILED_PHASE="coverage-floor python/bash drift: $_cov_drift"
  exit 3
fi
# --- end dual-run assertion ---
# The verdict: the module's exit code IS the decision. Exit 3 = coverage short
# OR registry-unusable (both retry, neither is a pass — what must never happen
# again is `== GATE PASS ==` over 31% coverage, #3002). FAILED_PHASE drives the
# trap's verdict.json / status file.
if [ "$_cov_rc" -ne 0 ]; then
  FAILED_PHASE=$(printf '%s\n' "$_cov_out" | sed -n 's/^@cov:failed_phase=//p' | head -1)
  FAILED_PHASE=${FAILED_PHASE:-coverage floor}
  exit "$_cov_rc"
fi

# Record the cache identity this pass populated the bincache for, so the NEXT
# gate can tell warm from cold (see the IDENTITY CHECK above). Best-effort —
# never fails a gate that already passed.
#
# The condition is `rc == 0`, i.e. the sharded pass ran to COMPLETION — not
# merely "a nonzero gtest total". A run killed at the shard ceiling still
# reports whatever it finished, so a nonzero total does NOT mean the store was
# fully populated: gate I on 2026-07-29 reported 5600 of 12775 (44%) and passed
# via arbitration, and under the old condition would have marked a 44%-populated
# store warm — re-creating, from the other direction, exactly the mis-prediction
# this marker exists to prevent.
if [ "${rc:-1}" -eq 0 ] && [ "${COV_GTESTS:-0}" -gt 0 ] && [ -n "${GATE_CACHE_ID:-}" ] && [ -n "${GATE_CACHE_ID_FILE:-}" ]; then
  # Phase 5 (#3010): write-marker is best-effort (mkdir + write + never raise),
  # matching the old `2>/dev/null` swallow. A no-op on empty identity.
  PYTHONPATH=scripts python3 -m gate.identity write-marker \
    --path "$GATE_CACHE_ID_FILE" --identity "$GATE_CACHE_ID" 2>/dev/null && \
    echo "== gate: recorded AOT cache identity for the next run's warm/cold prediction =="
fi

echo "== GATE PASS =="

# fix #2917 (follow-on to #2633): a SINGLE-mode gate is not an acceptance verdict.
#
# This script runs ONE oracle per invocation (MODE is the first arg only). #2633
# measured that the two are different oracles, not two strengths of one: ASan
# replaces the allocator and changes layout, so a wild read that FAULTS on the
# plain build can land in mapped memory and pass under --full. Same binary, same
# filter, in #2633's evidence: 3 failures plain, 0 under ASan.
#
# The banner above says only that THIS oracle is green. Without this note a log
# ending in `== GATE PASS ==` reads as a full acceptance when pasted into a
# report — which is how runs came to gate almost exclusively with --full,
# leaving that whole defect class invisible however many times the gate ran.
#
# Suppressed when scripts/gate_full_cycle.sh is driving (it runs both modes and
# applies its own acceptance condition), so the note appears exactly when the
# result really is partial.
if [ "${BEZEL_GATE_CYCLE:-0}" != 1 ]; then
  echo "== gate: NOTE — this is ONE oracle (mode=$MODE), NOT a full acceptance verdict. =="
  echo "==   #2633: plain and --full catch DIFFERENT defect classes; neither dominates. =="
  echo "==   To accept a batch (and to push), run scripts/gate_full_cycle.sh — or just  =="
  echo "==   scripts/gate_and_push.sh, which now gates on both oracles by default.      =="
fi
