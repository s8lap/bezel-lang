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
#                              #   ONE authoritative PLAIN sharded pass.
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
# NO clang-tidy here (fix #3666). The advisory clang-tidy tier used to be a
# gate.sh phase, i.e. it ran once per ORACLE: 277 s median per gate.sh run,
# ~547 s per two-oracle cycle (24% of a ~2230 s cycle), uncached, over
# identical inputs both times (the cycle pins HEAD, so the merge-base and the
# changed-file list cannot differ) — and nothing read its result (no trend
# rule, telemetry field or self-test names a `tidy` phase). It now runs ONCE
# per cycle in scripts/gate_full_cycle.sh, after the conformance sweep, still
# advisory. A standalone gate.sh run therefore does NO static analysis. By
# hand: `scripts/check_tidy.sh build --changed <base>` (base = the merge-base
# with main), or `cmake --build build --target check-tidy` for a full sweep.
#
# CANARY tier (gate[4/5], #2100): before the authoritative sharded pass, the
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
#   GATE_CANARY=0            skip the canary entirely. scripts/gate_full_cycle.sh
#                            sets this on its --full (ASan) pass — fix #3666:
#                            0 catches in 46 gate.sh runs, and on the ASan
#                            oracle a median 184 s (up to 286 s) whenever it
#                            ran, so its expected value there is negative. The
#                            cycle also passes BEZEL_GATE_ATTEMPT=<n> (echoed
#                            on the shard-ceiling line as `attempt=<n>`;
#                            `standalone` when unset) and BEZEL_GATE_ORACLE=
#                            plain|asan (the cycle's INTENDED oracle, for the
#                            log and child processes; gate.sh derives its own
#                            from MODE and the cycle checks the OBSERVED
#                            evidence — assert-asan-ran).
#   GATE_CANARY_BASE=<ref>   selection base (default: merge-base HEAD origin/main)
#   GATE_CANARY_MAX_PCT=<n>  skip when the selection exceeds n% of the suite
#                            (default 50 — the full pass then does the work anyway)
set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gate: not in a git repo" >&2; exit 2; }
# fix #3413: the tree this gate validates, recorded so its verdict names its own
# subject. gate.sh already follows the caller's tree (the cd above) — it was
# gate_full_cycle.sh that cd'd to a hardcoded path — so this is identity, not a
# behaviour change. `pwd -P` matches the spelling gate_full_cycle.sh and
# gate_and_push.sh compare against, so a symlinked lane path cannot read as a
# different tree.
#
# ⚠ DELIBERATELY NOT ECHOED. gate.sh's stdout is captured to the per-oracle log,
# and the ASan-ran evidence check is `grep -qi 'asan\|sanitize'` over the --full
# log (gate_full_cycle.sh, assert-asan-ran). A repo path containing "asan" —
# routine under /var/tmp/bezel-scratch/build-asan-* — would satisfy that grep
# from the PATH alone and turn the silent-degradation detector into a tautology.
# The identity banner therefore lives in gate_full_cycle.sh, whose stdout that
# grep never reads. The verdict JSON below is not grepped, so it is safe there.
GATE_REPO="$(pwd -P)"

# Make scripts/ importable as the `gate` package so `python3 -m gate.serialize`
# resolves from any cwd. Honor an operator's pre-set PYTHONPATH by appending
# rather than overwriting.
export PYTHONPATH="${PYTHONPATH:+${PYTHONPATH}:}scripts"
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
  local oracle="plain"; [ "${ASAN:-0}" = "1" ] && oracle="asan"
  # per the exit-code-trap / #3010 Wave 3: the truncation banner grep AND the 12
  # conditional `[ -n "${X+x}" ]` arg guards moved INTO verdict.py's
  # write_from_state. Python reads the shard log itself (the "shard crash /
  # undercount" banner grep is now an `in` check — ONE authority for run_parallel's
  # wording, not a bash re-implementation) and computes truncated/undercount;
  # the conditional guards collapse to `${VAR:-}` defaults + Python's empty->
  # None normalization (an unmeasured number still emits null, never 0, #3010).
  # Best-effort `|| true` preserved: a verdict-write hiccup NEVER changes the
  # gate's exit code. Dual-run equivalence (new verdict == old verdict) is pinned
  # by DualRunWriteFromStateTest over a matrix of reachable gate states.
  # --mode uses the `=` form because MODE can be `--full`/`--quick`/`--no-asan`,
  # and a bare `--mode --full` makes argparse treat `--full` as an unknown FLAG
  # (exit 2), silently skipping the verdict write (the `|| true` swallows it).
  PYTHONPATH=scripts python3 -m gate.verdict write_from_state \
    --path "$GATE_VERDICT_FILE" --rc "$rc" \
    --shard-log "$S/gate_shard.log" \
    --head "${GATE_HEAD:-}" --repo "${GATE_REPO:-}" --mode="${MODE:-}" --oracle "$oracle" \
    --started-epoch "${GATE_EPOCH:-0}" --total-s "$total" \
    --failed-phase "${FAILED_PHASE:-}" \
    --shard-count "${SHARD_COUNT:-0}" --shard-reported-s "${SHARD_REPORTED_S:-0}" \
    --coverage-gtests "${COV_GTESTS:-}" --coverage-littests "${COV_LITTESTS:-}" \
    --expected-gtests "${GATE_EXPECTED_GTESTS:-}" \
    --asan-errors "${ASAN_ERRS:-}" --arbitration-suspects "${ARB_SUSPECTS:-}" \
    2>/dev/null || true
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
# The gate builds with its own -j16 and never joins the shared build jobserver
# (scripts/build_jobserver.py): it runs with zero other builders, and a gate
# build must not depend on a daemon's health.
export BEZEL_JOBSERVER=off

# Zero-builders preflight (cold-cache flake defense): a --full gate run
# concurrently with even one producer build can stall a shard and trigger the
# cold-cache timeout flake (the #1 documented cause). Refuse to start if heavy
# compile/test comms are already active. Matched by process COMM (exact name) so
# it cannot self-match this bash script. Bypass with GATE_SKIP_BUILDER_CHECK=1.
# NB `bezel_tests(_[a-z]+)?` covers BOTH the historical single binary and the
# per-area binaries the suite is now built as (bezel_tests_codegen, …) — pgrep's
# pattern is a regex and -x anchors it to the WHOLE comm, so this still cannot
# match this bash script.
# NB `ninja-build`: Fedora's ninja binary is `/usr/bin/ninja-build` (a symlink
# target invoked directly, not via the `ninja` name), and the kernel sets a
# process's comm from the invoked pathname's basename — so a real ninja build
# has comm `ninja-build`, which the bare `ninja` alternative does NOT match
# under `-x`'s whole-string anchoring (same class of gap as #5080/#5083; #5087).
BUILDERS='cmake|cc1plus|ld|mold|lld|ninja|ninja-build|make|bezel_tests(_[a-z]+)?'
# Sibling detection for the preflight (gate-infra preflight-lockfile lane):
# when the ONLY matched builders are processes descended from another gate, do
# NOT report them as "builders active" — they are this gate's own workers, not
# a concurrent producer build. A second concurrent `gate_and_push.sh` must NOT
# be blocked by the first's `cmake`/`run_parallel.sh` children — that deadlock
# is the bug this whole lane exists to fix. The check uses /proc/<pid>/status
# to walk ancestors: a process whose ancestor chain includes any of the gate
# driver scripts is a sibling. If ANY matched builder has NO gate ancestor, the
# original "builders active" message fires (an actual orphan producer build IS
# active and a concurrent gate still risks the cold-cache flake).
#
# The /proc walk itself now lives in scripts/gate/serialize.py
# (`is_gate_sibling`, post-run infra migration; pinned by
# test_gate_serialization.py's SiblingDetectionTest). This wrapper is a thin
# shell adapter so the gate.sh call sites read identically to the bash
# implementation: returns 0 on yes, 1 on no. The verdict is captured first
# (the helper exits 1 on "no" — using `|| return 1` would short-circuit the
# assignment), then matched against "yes" via `[ ... ]`.
_is_gate_ancestor() { # $1 = pid  -> returns 0 iff any ancestor's comm is a gate driver
  local pid="$1" _verdict
  _verdict=$(PYTHONPATH=scripts python3 -m gate.serialize is-sibling --pid "$pid" 2>/dev/null)
  [ "$_verdict" = yes ]
}
if [ -z "${GATE_SKIP_BUILDER_CHECK:-}" ] && pgrep -x "$BUILDERS" >/dev/null 2>&1; then
  # Separate sibling-only from mixed (sibling + non-sibling) matches. Mixed
  # falls through to the original abort message — an orphan producer build IS
  # active alongside a sibling, so the cold-cache flake risk is unchanged.
  sibling_only=1; sibling_ppids=""
  while IFS= read -r _pid; do
    [ -n "$_pid" ] || continue
    if _is_gate_ancestor "$_pid" >/dev/null 2>&1; then
      sibling_ppids="$sibling_ppids $_pid"
    else
      sibling_only=0
    fi
  done < <(pgrep -x "$BUILDERS" 2>/dev/null)
  if [ "$sibling_only" -eq 1 ]; then
    echo "GATE ABORT: another gate is running (matched builders are all gate-and-push siblings; pids:${sibling_ppids}); wait for it to finish, or set GATE_SKIP_BUILDER_CHECK=1." >&2
    pgrep -ax "$BUILDERS" >&2 || true
    gate_write_verdict 2 0
    exit 2
  fi
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
  # The ASan pass caps at 8 shards. The ORIGINAL rationale (2026-07-17) was that
  # 16 concurrent ASan shards OOM-kill workers — "each ASan compile+exec peaks
  # ~2.5-3G RSS", sized against "the ~24-28G slice" — producing a spurious
  # "shard crash / undercount" FAIL over a different random set of tests each
  # run. The OBSERVATION was real (8 shards measured ~270-320s vs 16 shards
  # ~360-965s). The EXPLANATION was wrong twice over, and both errors were only
  # found when someone finally measured (2026-08-21):
  #
  #   1. WRONG CGROUP. A suite op does not run in the 24-28G slice; it runs in
  #      the `dev_heavy.sh suite` cage, which is 12G (now BEZEL_SUITE_MEM).
  #   2. NOT MEMORY-BOUND AT ALL. A full ASan pass peaks ~1 GiB of ANON memory
  #      against that 12G cage. The ~2.5-3G figure was memory.current, which is
  #      ~all reclaimable page cache (`anon 0 / file 14.3G` at idle) — the
  #      kernel reclaims it rather than OOM-killing for it.
  #
  # What actually bounds this pass is the AOT link-token pool:
  # aotLinkTokenCount() = min(physicalCores, MemAvailable/1.5GiB) = 10 here, so
  # shards past ~10 just block on an flock. Measured full suite, ASan, cold:
  # conc=8 634s vs conc=16 615s — 3%. The cap therefore stays at 8 because
  # raising it BUYS NOTHING, not because 16 is unsafe; and a larger-memory host
  # will not help. GATE_SHARDS still overrides. The plain pass keeps
  # run_parallel's nproc default (16), which is already past the token pool.
  #
  # scripts/gate/mempeak.sh samples the anon peak into the `mem` telemetry
  # field, so this stays checkable instead of becoming folklore again.
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
  # Wave 3 (#3010): the schema:2 record is now built by the gate.telemetry
  # Python module. The 25-field bash printf, the awk per-binary merge (with its
  # `exp`/`ran` gawk-builtin-names footgun), and the j_* null-vs-0 conditionals
  # are all ported there — a dict + json.dumps replaces the printf, a dict
  # union replaces the awk, and None-vs-0 (trivially correct in Python) replaces
  # the null-vs-0 rules. Proven byte-identical to the old bash over a controlled
  # dual-run (8 cases, every null-vs-0 branch, zero raw-byte diff). See
  # scripts/gate/telemetry.py + scripts/gate/tests/test_telemetry.py.
  #
  # fix #3010 (preserved by the module): AN UNMEASURED NUMBER EMITS null, NEVER
  # 0. A flag OMITTED below means the variable was unset (unmeasured => null);
  # a flag present with a value means measured. The per-field null-vs-0 RULE
  # (e.g. gtests null on 0, cache_n 0 on 0) lives in the module — it is the
  # exact `[ -n "${VAR:-}" ]` conditional the old j_* locals encoded.
  local _tsv="$S/.gate-phases.$$.tsv" i
  { for (( i=0; i<${#PH_NAME[@]}; i++ )); do
      printf '%s\t%s\t%s\t%s\t%s\n' "${PH_NAME[$i]}" "${PH_DUR[$i]}" "${PH_RC[$i]}" "${PH_STATUS[$i]}" "${PH_NOTE[$i]}"
    done; } > "$_tsv" 2>/dev/null
  # Always-present scalar fields (identity/context/shard) — `${VAR:-0}` matches
  # the old printf's unconditional defaults. NB `--mode="$MODE"` uses the `=`
  # form because MODE can be `--full`/`--quick`/`--no-asan`, and a bare
  # `--mode --full` makes argparse treat `--full` as an unknown FLAG (exit 2),
  # silently skipping the record (the `|| true` swallows it) — the same trap
  # gate_write_verdict documents at the mode flag (see :117-119).
  local -a a=(
    --phases-file "$_tsv" --tlog "$TLOG" --result "$result" --total-s "$total"
    --ts "$GATE_TS" --epoch "${GATE_EPOCH:-0}" --host "$GATE_HOST" --mode="$MODE"
    --head "$GATE_HEAD" --head-short "$GATE_HEAD_SHORT" --dirty "$GATE_DIRTY"
    --shard-times "${SHARD_TIMES:-}"
    --shard-count "${SHARD_COUNT:-0}" --shard-reported-s "${SHARD_REPORTED_S:-0}"
    --arbitration-suspects "${ARB_SUSPECTS:-0}"
    --serial-attempts "${SERIAL_ATTEMPTS:-0}" --serial-total "${SERIAL_TOTAL:-0}"
    --asan-shards-reported "${ASAN_SHARDS:-0}" --asan-errors "${ASAN_ERRS:-0}")
  # per #3010: conditionally-present fields — OMITTED when unset => null in the
  # record. cache_n/sto/GATE_COLD are set late (gate.sh:572-578) so are unset on
  # early-exit paths; COV_*/AOT_*/GATE_BIN_* are set even later by the harvest.
  [ -n "$FAILED_PHASE" ] && a+=(--failed-phase "$FAILED_PHASE")
  [ -n "${COV_GTESTS:-}" ] && a+=(--cov-gtests "$COV_GTESTS")
  [ -n "${COV_LITTESTS:-}" ] && a+=(--cov-littests "$COV_LITTESTS")
  [ -n "${GATE_EXPECTED_GTESTS:-}" ] && a+=(--expected-gtests "$GATE_EXPECTED_GTESTS")
  [ -n "${GATE_FLOOR_OK:-}" ] && a+=(--coverage-floor-ok "$GATE_FLOOR_OK")
  [ -n "${GATE_COLD:-}" ] && a+=(--cold "$GATE_COLD")
  [ -n "${cache_n:-}" ] && a+=(--cache-n "$cache_n")
  [ -n "${sto:-}" ] && a+=(--shard-timeout "$sto")
  [ -n "${AOT_HITS:-}" ] && a+=(--aot-hits "$AOT_HITS")
  [ -n "${AOT_MISSES:-}" ] && a+=(--aot-misses "$AOT_MISSES")
  [ -n "${AOT_STORE_FAILS:-}" ] && a+=(--aot-store-fails "$AOT_STORE_FAILS")
  [ -n "${MEM_ANON_MAX:-}" ] && a+=(--mem-anon-max "$MEM_ANON_MAX")
  [ -n "${MEM_CURRENT_MAX:-}" ] && a+=(--mem-current-max "$MEM_CURRENT_MAX")
  [ -n "${MEM_SAMPLES:-}" ] && a+=(--mem-samples "$MEM_SAMPLES")
  [ -n "${GATE_BIN_EXPECTED:-}" ] && a+=(--bin-expected "$GATE_BIN_EXPECTED")
  [ -n "${GATE_BIN_RAN:-}" ] && a+=(--bin-ran "$GATE_BIN_RAN")
  PYTHONPATH=scripts python3 -m gate.telemetry emit "${a[@]}" > /dev/null 2>&1 || true
  rm -f "$_tsv" 2>/dev/null || true
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
# binaries the gate accounts for is exactly the set CMake built. The manifest is
# now read + verified by the gate.manifest module (Wave 3, #3010): the `verify`
# call after the build phase replaces the old `test_binaries()` (`cut -f2`)
# reader + the per-binary `[ -x ]` loop.

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
  return "$_verdict"
}

# fix #3667: the per-shard logs run_parallel KEPT for THIS run. On a nonzero
# exit run_parallel's _rp_keep trap (tests/run_parallel.sh:101-115) moves its
# logdir to ${BEZEL_SHARD_LOG_KEEP:-/var/tmp/bezel-scratch/shard-logs}/<YYYYmmdd-
# HHMMSS>-rc<N> and says only the ROOT on stderr — `per-shard logs KEPT in
# <root>/ (see #3010)` — which lands in the sharded-run log. So: read the root
# off that line, then take the newest `*-rc*` directory under it that is NEWER
# than the stamp gate.sh touched before launching run_parallel. Never the root
# itself and never an older sibling: the root holds every non-clean run ever
# kept (measured 2026-09-03: 249 directories, 217 of them carrying a `[ FAIL ]`
# in some shard log), so a scan over it would veto every future ceiling-only
# retry with a failure from a run that is not this one. Echoes the directory,
# or nothing (no KEPT line — the logs were reaped or the mv failed; or nothing
# newer than the stamp); the caller then classifies on the gate log alone and
# says so. gate.summary documents reaped logs as "no evidence, not a defect"
# (ceiling_only_failure). Never fails.
gate_shard_logdir() { # $1 = sharded-run log, $2 = stamp file → echoes the kept logdir, or nothing
  local log="$1" stamp="$2" root d
  root=$(sed -n 's/.*per-shard logs KEPT in \(.*\)\/ (see #3010).*/\1/p' "$log" 2>/dev/null | tail -1)
  [ -n "$root" ] && [ -d "$root" ] && [ -n "$stamp" ] && [ -f "$stamp" ] || return 0
  d=$(find "$root" -mindepth 1 -maxdepth 1 -type d -name '*-rc*' -newer "$stamp" 2>/dev/null | sort | tail -1)
  [ -n "$d" ] && printf '%s\n' "$d"
  return 0
}

# fix #4533: the gate's side of the sanitizer-evidence channel. ONE reader for
# BOTH sharded consumers (the authoritative pass and the canary tier), because
# both launch tests/run_parallel.sh and both used to grep its stdout for a
# headline that stream structurally cannot carry.
#
# The decision itself lives in scripts/gate/sanitizer.py — it is a real
# decision (corruption in a FAILING test is deterministic evidence; corruption
# in a PASSING test is a defect unless the row declares that its program's
# crash is the specified outcome; an unattributable report is never excused;
# a leak is counted, not fatal) and it belongs where it can be unit-tested,
# not in a bash conditional. Exit codes: 0 clean, 1 FATAL, 2 the channel is not
# wired — which must never read like a clean suite.
gate_sanitizer_verdict() { # $1 = sharded-run log → prints findings; rc 0/1/2
  PYTHONPATH=scripts python3 -m gate.sanitizer verdict --shard-log "$1" 2>&1
}

echo "== gate[1/5]: reconfigure (glob any new test files) =="
# Snapshot the compiler-archive mtime BEFORE any build work: comparing after
# the build phase tells us whether the k2 cache identity changed this gate
# (core.a rebuilt => cold ASan pass guaranteed) — see the ceiling note above.
CORE_MTIME_PRE=$(stat -c %Y build/libbezel_core.a 2>/dev/null || echo 0)

# AOT bincache + workdir setup is the operator's responsibility — see
# CLAUDE.md "AOT bincache + workdir must be on the fast disk too" and
# scripts/lane_setup.sh which creates the /mnt/fast symlinks idempotently.
# We intentionally do NOT probe fsync speed or filesystem type here:
# thresholds are machine-dependent and would false-positive on a busy but
# fast disk (NVMe under contention is ~5ms, not 65ms btrfs) or miss
# an unfamiliar slow backend (xfs-on-SATA, USB drive, network mount).
# The cold gate's behavior under a slow cache speaks for itself; if you
# see per-test shard phases hitting the 1800s ceiling with runs/OK
# counts in the 300s, the bincache is the first thing to check.
PHASE_T0=$(now)
"$DH" build cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$S/gate_reconf.log" 2>&1; prc=$?
rec_ran configure "$prc"
[ "$prc" -ne 0 ] && { tail -15 "$S/gate_reconf.log"; fail "configure"; }

# `bezel_tests` is the AGGREGATE target over the per-area binaries
# (tests/CMakeLists.txt) — the target name is unchanged, so this phase is too.
echo "== gate[2/5]: build bezel_tests (all per-area binaries) =="
PHASE_T0=$(now)
"$DH" build cmake --build build --target bezel_tests -j16 >"$S/gate_build.log" 2>&1; prc=$?
rec_ran build "$prc"
[ "$prc" -ne 0 ] && { grep -iE 'error:|FAILED:' "$S/gate_build.log" | head -20; fail "build"; }
# A missing/empty manifest means the gate has no idea what to run — refuse
# rather than silently sharding a subset of the suite.
[ -s build/tests/test_binaries.txt ] || fail "build produced no test-binary manifest (build/tests/test_binaries.txt)"
# per #3011: the manifest module reads the manifest AND verifies every binary
# is executable, hard-failing on missing/non-executable (the gate's own input
# was the defect it catches — never silently lower the bar). Replaces the old
# `cut -f2` + per-binary `[ -x ]` loop. Outputs the verified paths one per line;
# emits NOTHING to stdout on failure so the loop below never iterates a partial
# set.
if ! _gate_bins=$(PYTHONPATH=scripts python3 -m gate.manifest verify --path build/tests/test_binaries.txt); then
  fail "build reported success but a test binary is missing or not executable (manifest verify failed — see module stderr above)"
fi
while IFS= read -r _b; do
  [ -n "$_b" ] || continue
  echo "   binary: $(basename "$_b")  $(ls -la --time-style=+%H:%M:%S "$_b" | awk '{print $6}')"
done <<< "$_gate_bins"

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
# marker are the gate.identity / gate.cold Python modules. Python is
# AUTHORITATIVE; the dual-run bash recompute that proved equivalence has been
# removed (the migration is complete). The invariants the modules encode are
# UNCHANGED — #2100 (the 3-signal cold predicate), #2918 (the per-oracle marker),
# #3000 (find -L) — see the module docstrings and the block comments above.
oracle="plain"; [ "$ASAN" -eq 1 ] && oracle="asan"
GATE_CACHE_ID=$(PYTHONPATH=scripts python3 -m gate.identity compute \
  --core build/libbezel_core.a --runtime build/libbezel_runtime.a 2>/dev/null)
# per #2918: the marker filename MUST be per-oracle (plain/ASan are disjoint
# populations in one store); marker_filename encodes that branch.
GATE_CACHE_ID_FILE=$(PYTHONPATH=scripts python3 -m gate.identity marker-filename \
  --cache-root "$GATE_CACHE_ROOT" --oracle "$oracle" 2>/dev/null)
_recorded_id=$(PYTHONPATH=scripts python3 -m gate.identity read-marker --path "$GATE_CACHE_ID_FILE" 2>/dev/null)
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
# fix #3222: THE CEILING WAS ORACLE-BLIND; THE WORKLOAD IS NOT.
#
# `gate.cold` predicts cold/warm from the AOT cache and returns one ceiling,
# which both oracles then used unchanged. But an ASan pass instruments every
# generated test program, so its shard workload is roughly TWICE the plain
# pass's. Measured on the 2026-08-14 Wave-B2 gate, same tree, same box, both
# cold-predicted at 1800s:
#
#     plain shard phase   1018.30s   rc=0   (comfortable)
#     ASan  shard phase   1915.61s   rc=1   (all 8 codegen shards hit 124)
#
# Every codegen shard had completed ~1400 of its ~1478 tests — about 95% — and
# was killed at the ceiling. That is not a hang and not a real failure; it is a
# budget calibrated on the cheaper oracle and applied to the dearer one, and it
# cost a full 32-minute pass plus a re-run. It also mis-reports: a timed-out
# shard writes no JSON summary, so the headline became "shard-summary MISMATCH"
# rather than "shard timed out".
#
# The asymmetry is what decides the multiplier. This timeout exists to kill a
# DEADLOCKED shard (`tests/run_parallel.sh:143`). A deadlocked shard is caught
# just as surely at 2x the ceiling — merely 30 minutes later, once per hang,
# which is rare. A ceiling set too LOW costs a whole cycle every time the suite
# grows past it, which is continuous. So the failure costs are wildly asymmetric
# and the multiplier should be generous rather than snug: 2x, not the 1.06x that
# would have just cleared this run.
ASAN_STO_MULT="${BEZEL_ASAN_SHARD_TIMEOUT_MULT:-2}"
if [ "$ASAN" -eq 1 ]; then
  ceiling=$(( ceiling * ASAN_STO_MULT ))
fi
# A caller's explicit BEZEL_SHARD_TIMEOUT always wins (see GATE_EXPLICIT_STO).
sto="$GATE_EXPLICIT_STO"; [ -z "$sto" ] && sto=$ceiling
SHARD_ENV+=(BEZEL_SHARD_TIMEOUT="$sto")
# fix #3690: the gate's bincache stores run WITHOUT the #3009 fsync pair
# (BEZEL_AOT_STORE_FSYNC=0, #3669). The harness compensates with a `<entry>.sz`
# size sidecar that installFromCache verifies on EVERY read — a size
# disagreement is a counted miss (`verify_rejects=` on the [aot-cache] line),
# never a run of a torn executable — so the property #3009 bought survives by
# verification instead of durability. Measured before the flip:
# store_fsyncs=27195 on the last ASan pass (2026-09-03), about twice that in
# fsync(2) calls, ~4% of a cold cycle, paid every cycle (46/46 gates cold).
# The verification is SIZE-ONLY (a same-size corruption is not caught), which
# is a slightly weaker guarantee than #3009 shipped, accepted in #3690.
#
# PLACEMENT IS LOAD-BEARING. This must FOLLOW the ASan branch above, which
# REASSIGNS SHARD_ENV (a `+=` up in the seeding block is silently dropped on
# every --full pass — the pass with ~27k stores), and PRECEDE both run sites
# (canary and main) so every store the gate makes is in one mode and
# `store_fsyncs=0` on the aggregate line is a checkable entry criterion.
# Evidence lives on that line in $S/gate_shard.log (store_fsync_skipped= /
# verify_rejects=, #3669) and is deliberately NOT a telemetry key
# (test_telemetry pins the 29-key record). scripts/gate/cold.py count_cache
# excludes the sidecars, or cache_n would double toward the TIGHTER (warm,
# 900s) ceiling. gate_arbitrate's re-runs build their own env and so store in
# the default (fsync) mode; that is harmless — verify-on-read is unconditional
# on the reader — and costs one fsync pair per arbitrated suspect.
SHARD_ENV+=(BEZEL_AOT_STORE_FSYNC=0)
core_note="unchanged"; [ "$core_post" != "$CORE_MTIME_PRE" ] && core_note="rebuilt"
cold_note="warm-predicted"; [ "$GATE_COLD" -eq 1 ] && cold_note="cold-predicted"
_sto_label="plain"; [ "$ASAN" -eq 1 ] && _sto_label="ASan"
_mult_note=""; [ "$ASAN" -eq 1 ] && _mult_note=", x${ASAN_STO_MULT} for ASan (#3222)"
# fix #3666: the cycle's attempt number (BEZEL_GATE_ATTEMPT) rides on this line
# so a per-attempt log names its own attempt; a standalone run says so. On
# --full this line is also the "ASan" evidence assert-asan-ran greps for
# (via $_sto_label) — it must keep carrying that word.
echo "   $_sto_label shard ceiling: ${sto}s ($cold_note: core.a $core_note, bincache entries=$cache_n, identity $gate_id_note$_mult_note) attempt=${BEZEL_GATE_ATTEMPT:-standalone}"
# fix #3690: say which store mode the shards run in, beside the ceiling they
# run under; `bincache entries=` above is now an ENTRY count (no sidecars).
echo "   store fsync: OFF (verify-on-read, #3669)"

# lit + FileCheck diagnostic track (tests/lit/): drives the REAL bezelc over .bzl
# files and matches diagnostics with FileCheck. Fast (~0.1s/test, no JIT) and
# per-process, so it validates the accept/reject + diagnostic-text/location
# surface without the sharding-pollution or wrong-harness (#1682) failure modes
# the gtest suite is exposed to. Skipped cleanly if lit/FileCheck/not were absent
# at configure (the site config is only generated when all three are found).
echo "== gate[3/5]: lit diagnostic track (real bezelc + FileCheck, per-process) =="
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
echo "== gate[4/5]: canary — diff-selected subset (fail-fast only; never acceptance) =="
CANARY_LIST="$S/gate_canary.list"
canary_skip() { echo "   (canary skipped: $1)"; rec_skip canary "$1"; }
# per #3010 Wave 4: the canary DECISION logic (the classify/dispatch) is now
# gate.canary.decide (#2100). DUAL-RUN: compute the decision BOTH ways (the old
# bash dispatch below + Python canary.decide) and assert 0 disagreement — the
# old bash is the authority until proven. A divergence is a migration bug -> fail
# loudly (never silently trust either side). The SPAWN (the run_parallel
# invocation at :652) stays bash glue — only the DECISION moved to Python.
# _c_* track the decide() inputs as the bash chain progresses (initialized to
# the not-yet-reached defaults: selok=1, arb=-1=not-arbitrated, asan=0).
_c_selok=1; _c_nsel=0; _c_nuniv=0
_c_maxpct="${GATE_CANARY_MAX_PCT:-50}"; _c_asan=0; _c_arb=-1
_canary_dual() { # $1 = the OLD bash dispatch token (run|skip|hard)
  local _py
  _py=$(PYTHONPATH=scripts python3 -m gate.canary decide \
    --nsel "$_c_nsel" --nuniv "$_c_nuniv" --max-pct "$_c_maxpct" \
    --asan-hits "$_c_asan" --arb-verdict "$_c_arb" \
    $([ "$_c_selok" -eq 0 ] && echo --no-selector-ok) 2>/dev/null) || _py="ERR"
  if [ "$1" != "$_py" ]; then
    echo "   !! CANARY DUAL-RUN DISAGREEMENT: bash=$1 python=$_py" \
         "(nsel=$_c_nsel nuniv=$_c_nuniv maxpct=$_c_maxpct asan=$_c_asan arb=$_c_arb selok=$_c_selok)" >&2
    echo "   !! The Python canary.decide diverged from the old bash dispatch — see scripts/gate/tests/test_canary.py" >&2
    fail "canary dual-run disagreement (bash=$1 python=$_py) — migration bug"
  fi
}
if [ "${GATE_CANARY:-1}" = "0" ]; then
  canary_skip "GATE_CANARY=0"
else
  PHASE_T0=$(now)
  cbase="${GATE_CANARY_BASE:-}"
  [ -z "$cbase" ] && cbase="$(git merge-base HEAD origin/main 2>/dev/null || true)"
  if [ -z "$cbase" ]; then
    _c_selok=0; _canary_dual skip
    canary_skip "no base (origin/main unavailable and no GATE_CANARY_BASE)"
  elif ! python3 scripts/select_tests.py --base "$cbase" -o "$CANARY_LIST" >"$S/gate_canary_select.log" 2>&1; then
    tail -5 "$S/gate_canary_select.log"
    _c_selok=0; _canary_dual skip
    canary_skip "selector failed (fail-open; see $S/gate_canary_select.log)"
  else
    nsel=$(grep -c . "$CANARY_LIST" 2>/dev/null); nsel=${nsel:-0}; _c_nsel="$nsel"
    nuniv=$(grep -oE 'universe: [0-9]+' "$S/gate_canary_select.log" | grep -oE '[0-9]+' | tail -1); nuniv=${nuniv:-0}; _c_nuniv="$nuniv"
    maxpct="${GATE_CANARY_MAX_PCT:-50}"; _c_maxpct="$maxpct"
    if [ "$nsel" -eq 0 ]; then
      _canary_dual skip
      canary_skip "empty selection (no mapped src/test diff vs base)"
    elif [ "$nuniv" -gt 0 ] && [ $(( 100 * nsel / nuniv )) -ge "$maxpct" ]; then
      _canary_dual skip
      canary_skip "selection $nsel/$nuniv >= ${maxpct}% of suite — full pass covers it"
    else
      echo "   canary: $nsel of $nuniv tests selected (base $(git rev-parse --short "$cbase" 2>/dev/null || echo "$cbase"))"
      grep -E '^signals:' "$S/gate_canary_select.log" | head -1 || true
      "$DH" suite "${SHARD_ENV[@]}" env BEZEL_TEST_LIST="$CANARY_LIST" tests/run_parallel.sh ${GATE_SHARDS:-} >"$S/gate_canary.log" 2>&1; crc=$?
      rec_ran canary "$crc" "$nsel/$nuniv selected${SHARD_NOTE:+, $SHARD_NOTE}"
      grep -E 'TOTAL:|WARNING: only' "$S/gate_canary.log" || true
      # A fired generated-code sanitizer report in the canary is DETERMINISTIC
      # evidence, exactly as in the full pass — fail hard, no arbitration.
      # fix #4533: THE CANARY LAUNCHES tests/run_parallel.sh TOO (just above), so
      # this grep had the SAME defect as the authoritative pass — it read
      # run_parallel's stdout, which a shard's program output never reaches.
      # (#4533 records the canary as unaffected "because it reads a direct
      # binary run"; that is not what the line above does.) It now reads the
      # same lifted channel, through the same reader, and the same judgement:
      # corruption in a passing row is evidence unless the row declares itself
      # in sanitizer_exempt.txt; a LEAK in a passing row is evidence unless the
      # row declares itself in sanitizer_leak_exempt.txt (fix #4537 — the 11
      # `leaksAtLeast` rows do, so a diff that touches only those does not
      # hard-fail a healthy canary).
      #
      # gate.canary.decide's contract is `asan_hits > 0 -> FAIL_HARD`; WHICH
      # reports count as evidence is the bash side's job (canary.py: "what stays
      # bash — the ASan check"), so handing it the FATAL verdict as a 1 keeps
      # the dual-run consistent without changing the module.
      if [ "$ASAN" -eq 1 ]; then
        _c_find=$(gate_sanitizer_verdict "$S/gate_canary.log"); _c_rc=$?
        if [ "$_c_rc" -eq 1 ]; then
          _c_asan=1
          echo "   -- generated-code ASan fired in the canary --"
          printf '%s\n' "$_c_find" | sed 's/^/   /'
          sed -n '/^--- sanitizer reports ---/,$p' "$S/gate_canary.log" | head -60
          _canary_dual hard
          fail "canary ASan — memory error in a diff-selected test program (see $S/gate_canary.log)"
        elif [ "$_c_rc" -ne 0 ]; then
          # The canary is FAIL-OPEN by design and is never the acceptance
          # authority. Degrade to "not evaluated here" and say so — the
          # authoritative pass below REFUSES an unwired channel, which is where
          # that refusal belongs.
          echo "   canary: sanitizer-evidence channel not readable — the ASan leg is not evaluated here; the authoritative pass refuses an unwired channel (#4533)"
          _c_asan=0
        else
          _c_asan=0
          [ -n "$_c_find" ] && printf '%s\n' "$_c_find" | sed 's/^/   canary: /'
        fi
      fi
      if [ "$crc" -ne 0 ]; then
        PHASE_T0=$(now)  # canary arbitration is its own (exceptional) phase
        gate_arbitrate "$S/gate_canary.log"; carb=$?; _c_arb="$carb"
        case "$carb" in
          0) _canary_dual run
             rec_ran canary_arbitration "0" "$ARB_LAST_SUSPECTS suspects, all flaky"
             echo "   all canary suspects flaky (pass alone within 5 tries) -> continue to the full pass";;
          2) _canary_dual skip
             # No arbitrable crash/FAIL marker: an INFRA shape — all-phantom
             # list (subset_empty), a shard dead before any test, an
             # undercount, a launch hiccup. Per the canary contract this is
             # FAIL-OPEN: warn loudly and continue — the authoritative full
             # pass follows unconditionally and catches anything real. (A
             # genuine failing/hanging selected test produces a died-on/FAIL
             # marker and takes the arbitrated paths instead.)
             rec_ran canary_arbitration "0" "no crash/FAIL marker — infra shape, fail-open"
             grep -E '## GATE FAILURE|SUBSET' "$S/gate_canary.log" | head -3 || true
             echo "   canary: nonzero exit with NO crash/FAIL marker (infra/selector shape) -> FAIL-OPEN to the full pass (see $S/gate_canary.log)";;
          *) _canary_dual hard
             rec_ran canary_arbitration "1" "$ARB_LAST_SUSPECTS suspects, real"
             fail "canary (diff-selected subset) — regression caught BEFORE the full pass (see $S/gate_canary.log)";;
        esac
      else
        _canary_dual run
      fi
    fi
  fi
fi

echo "== gate[5/5]: sharded suite ($SHARD_LABEL) — the authoritative oracle =="
PHASE_T0=$(now)
# per #3012 Phase 2c: name a merged run-summary path for the MAIN shard run
# only (not the canary above — its summary would otherwise clobber this one).
# run_parallel writes it; the harvest below reads it instead of grepping the
# printed TOTAL / per-binary lines. $S is /var/tmp/bezel-scratch (line 76).
GATE_RUN_SUMMARY="$S/gate_run_summary.json"
SHARD_ENV+=(BEZEL_RUN_SUMMARY="$GATE_RUN_SUMMARY")
# per gate audit F1: sample the dev_heavy slice's ANON high-water mark across
# the shard phase. ANON (+shmem+slab), not memory.current — the latter is ~all
# RECLAIMABLE page cache here (measured `anon 0 / file 14.3G` on an idle slice),
# so sizing anything against it overstates load by more than 10x. Anon is what
# OOM-kills a shard.
#
# MEASURED 2026-08-21, and the answer was "memory is not the constraint": a full
# ASan pass peaks ~1 GiB anon against this cage's 12G, and doubling shards
# (conc=8 -> 16) changed wall by 3%. The binding limit is the link-token pool
# (min(physicalCores, RAM/1.5GiB) = 10 here), not the cage. Kept sampling so the
# claim stays checkable as the suite and the box change — this is the number the
# conc=8 cap was sized against without anyone ever having measured it.
#
# Sampled, not queried after the fact: the run's scope is a transient unit
# systemd releases on exit, so a postmortem `systemctl show -p MemoryPeak` reads
# nothing. PURE OBSERVATION — best-effort, holds nothing, fails nothing.
MEMPEAK_OUT="$S/gate_mempeak.env"; MEMPEAK_PID=""
rm -f "$MEMPEAK_OUT" 2>/dev/null || true   # never source a PRIOR gate's numbers
if [ -x scripts/gate/mempeak.sh ]; then
  MEMPEAK_PID=$(scripts/gate/mempeak.sh start "$MEMPEAK_OUT" 2 2>/dev/null || true)
fi
# fix #3667: stamp the launch so gate_shard_logdir() (below) can pick the
# per-shard logdir run_parallel keeps for THIS run and never a previous gate's.
GATE_SHARD_STAMP="$S/gate_shard.stamp"; : > "$GATE_SHARD_STAMP"
# sharpen #3667 (review): GATE_RUN_SUMMARY is a FIXED path every gate
# overwrites. If run_parallel dies before `account` writes it
# (tests/run_parallel.sh:233), the ceiling-only classifier below — and the
# coverage harvest after it — would read the PREVIOUS gate's summary and
# classify a run that is not this one. Same rule as MEMPEAK_OUT above.
rm -f "$GATE_RUN_SUMMARY" 2>/dev/null || true   # never source a PRIOR gate's numbers
"$DH" suite "${SHARD_ENV[@]}" tests/run_parallel.sh ${GATE_SHARDS:-} >"$S/gate_shard.log" 2>&1; rc=$?
if [ -n "${MEMPEAK_PID:-}" ]; then
  scripts/gate/mempeak.sh stop "$MEMPEAK_PID" 2>/dev/null || true
  # shellcheck disable=SC1090
  [ -s "$MEMPEAK_OUT" ] && . "$MEMPEAK_OUT" 2>/dev/null || true
  MEM_ANON_MAX="${mem_anon_max_bytes:-}"; MEM_CURRENT_MAX="${mem_current_max_bytes:-}"
  MEM_SAMPLES="${mem_samples:-}"
  scripts/gate/mempeak.sh report "$MEMPEAK_OUT" 2>/dev/null | sed 's/^/== gate: /' || true
fi
# When ASan is on (--full), the shard phase's recorded duration INCLUDES the
# ASan compile+run (SHARD_NOTE marks it), so the ASan cost is captured in the
# phases array here — there is no separate asan phase.
rec_ran shard "$rc" "$SHARD_NOTE"
# Harvest run_parallel's own aggregate wall + shard count + per-shard times (obs only).
SHARD_COUNT=$(grep -oE 'across [0-9]+ shards' "$S/gate_shard.log" | grep -oE '[0-9]+' | tail -1); SHARD_COUNT=${SHARD_COUNT:-0}
SHARD_REPORTED_S=$(grep -oE 'across [0-9]+ shards, [0-9]+s' "$S/gate_shard.log" | grep -oE '[0-9]+s' | tr -d 's' | tail -1); SHARD_REPORTED_S=${SHARD_REPORTED_S:-0}
# fix #3667: the harvest is gate.summary's `shard-times` CLI. The grep it
# replaces was anchored `s$`, and run_parallel.sh:388 appends ` (ceiling 1800s,
# 0.7%)` whenever BEZEL_SHARD_TIMEOUT is set — which a gate run ALWAYS sets
# (the ceiling block above) — so since d0d2390 (2026-08-18, which added that
# suffix) it has matched nothing: of the 940 telemetry records on the box this
# was measured on, 690 carry shard_times data and the 188 written after
# 2026-08-18T06:05Z are all `"shard_times":[]`. Same key, same comma-joined
# shape; the CLI emits the RAW seconds tokens (`1795.50`, not a re-rendered
# float) because telemetry.py wraps each verbatim as a RawNumber. Empty on an
# unreadable log, exactly as before (observation only — never fails a gate).
SHARD_TIMES=$(PYTHONPATH=scripts python3 -m gate.summary shard-times "$S/gate_shard.log" 2>/dev/null || true)
# fix #3674: `actual FAIL` joins the alternation so the culprit a COMPLETED
# shard names — `##   shard X exited nonzero — completed its slice; actual
# FAIL: <test>` (#3668) and the older `##   shard X actual FAIL: <test>`
# (#3018) — reaches the reader. Before, a completed-shard failure condensed to
# the banner and the `--- failing tests ---` header and named no test at all.
grep -E 'TOTAL:|## GATE FAILURE|died on|actual FAIL|--- failing' "$S/gate_shard.log" || true
# ASan on (--full / GATE_FAST_ASAN=1): a fired generated-code sanitizer report is
# DETERMINISTIC evidence of a memory error — fail hard here, independent of the
# flake arbitration (which re-runs a suspect alone and, absent this, could clear
# an ASan-only failure). The harness re-emits any report with the '[aot-asan
# #1822]' banner; also catch the raw sanitizer headline. Skipped entirely in the
# plain default (a clean suite never matches anyway).
#
# fix #4533: THIS PHASE READ THE WRONG FILE AND SO COULD NEVER FIRE.
# `$S/gate_shard.log` is tests/run_parallel.sh's OWN STDOUT; a shard's PROGRAM
# output goes to a per-shard file under run_parallel's mktemp logdir, and only
# `^[ FAIL ]` lines were lifted back into the parent stream. The grep below was
# therefore over a file that STRUCTURALLY CANNOT hold the pattern, and a grep
# that returns 0 for that reason is indistinguishable from a clean suite.
# Measured on the failed 2026-09-21 --full cycle at e89577e4: seven programs
# printed `ERROR: LeakSanitizer` and
# `grep -cE 'ERROR: (Address|Leak)Sanitizer' e89577e4_full.log` → 0, while this
# phase recorded `rc=0 (no ASan report)`.
#
# run_parallel.sh now LIFTS the evidence out of the per-shard logs while they
# still exist (it reaps them on a clean run) and prints one unconditional
# accounting line. Two things changed here:
#   * LIVENESS IS ASSERTED. A missing accounting line fails the phase instead of
#     reading as "clean" — a guard that cannot fire is the defect being fixed,
#     so the gate must refuse a zero it did not actually measure.
#   * THE HARD-FAIL KEYS ON CORRUPTION AND ON UNDECLARED LEAKS. `ERROR:
#     AddressSanitizer` (UAF / OOB / double-free) is never deliberate in this
#     tree and fails the cycle on the spot, independent of rc and of
#     arbitration, UNLESS the test declares itself in
#     scripts/gate/sanitizer_exempt.txt (its crash is the spec's SPECIFIED
#     outcome). A LEAK headline (`ERROR: LeakSanitizer`) is fatal by the
#     identical rule against a SEPARATE list, sanitizer_leak_exempt.txt
#     (fix #4537): 37 rows across 5 distinct deliberate-leak-assertion shapes
#     (not just `leaksAtLeast` — the naive grep for that alone finds only 11;
#     see the file's own header for the other four) re-emit a real LSan report
#     while PASSING, and declare themselves there — every other leak now reds
#     the cycle by name, same as undeclared corruption. Both lists are read by
#     scripts/gate/sanitizer.py's verdict(); gate_sanitizer_verdict() below
#     never passes --exempt/--leak-exempt, so the DEFAULT paths (computed next
#     to sanitizer.py) are what the gate actually consults.
if [ "$ASAN" -eq 1 ]; then
  PHASE_T0=$(now)  # shard_asan is only the harvest; the ASan compile+run time is
                   # already attributed to the 'shard' phase above (no double-count).
  # The evidence line is echoed INSIDE this block on purpose: it carries the
  # substring `SANITIZER`, and gate_full_cycle.sh's silent-degradation detector
  # is `grep -qi 'asan\|sanitize'` over gate.sh's STDOUT for the --full pass.
  # Printing it unconditionally (in both oracles) would satisfy that grep from a
  # PLAIN run and turn the detector into a tautology — the same trap the
  # repo-identity banner near the top of this file documents (it is deliberately
  # not echoed for exactly that reason).
  asan_ev=$(grep -m1 '^SANITIZER EVIDENCE (all shards):' "$S/gate_shard.log" 2>/dev/null || true)
  [ -n "$asan_ev" ] && echo "   $asan_ev"
  asan_hits=$(printf '%s\n' "$asan_ev" | sed -n 's/^SANITIZER EVIDENCE (all shards): \([0-9][0-9]*\) report marker.*/\1/p')
  ASAN_ERRS=${asan_hits:-0}; ASAN_SHARDS=$SHARD_COUNT
  asan_scanned=$(printf '%s\n' "$asan_ev" | sed -n 's/^.* of \([0-9][0-9]*\) shard log(s) scanned.*/\1/p')
  asan_find=$(gate_sanitizer_verdict "$S/gate_shard.log"); asan_rc=$?
  [ -n "$asan_find" ] && printf '%s\n' "$asan_find" | sed 's/^/   /'
  case "$asan_rc" in
    0) rec_ran shard_asan "0" "no fatal finding (${asan_hits:-0} marker(s), ${asan_scanned:-0} shard log(s) scanned)" ;;
    1) # The verdict above NAMED the offending tests. Show the reports too: the
       # excerpt is the lifted section, which the scan ordered corruption-first.
       sed -n '/^--- sanitizer reports ---/,$p' "$S/gate_shard.log" | head -60
       rec_ran shard_asan "1" "corruption in the sharded pass"
       fail "sharded ASan — memory error in a test program (see $S/gate_shard.log)" ;;
    2) rec_ran shard_asan "1" "evidence channel unwired"
       fail "sharded ASan — the sanitizer-evidence channel is not wired; a zero from it is not a clean suite (#4533)" ;;
    # ANY other status is the reader itself failing (no python3, an import
    # error, a traceback). That must never fall through as "clean" — the whole
    # defect being fixed is a verification leg that reads green while switched
    # off.
    *) rec_ran shard_asan "1" "evidence reader exited $asan_rc"
       fail "sharded ASan — the sanitizer-evidence reader exited $asan_rc; its silence is not a clean suite (#4533)" ;;
  esac
fi
if [ "$rc" -ne 0 ]; then
  PHASE_T0=$(now)  # the flake-arbitration re-runs are their own (exceptional) phase
  # fix #3667: a run whose ONLY defect is a per-shard ceiling kill is RETRYABLE
  # infra, not a red suite. The kill leaves the AOT bincache ~95% warm, so a
  # retry costs ~310 s plain / ~500 s ASan against a fresh ~2230 s cycle plus a
  # relaunch — and until now gate_arbitrate found no marker in a log
  # run_parallel never let carry one, returned 2, the verdict was FAIL and the
  # cycle BAILed. The classifier is gate.summary's ceiling_only_failure — a
  # conjunction (a ceiling kill AND no crash AND no empty subset AND every
  # missing summary explained by the kill AND no `[ FAIL ]` in the gate log or
  # this run's kept per-shard logs). Its exit codes: 0 RETRYABLE (never a
  # generic success), 1 not, 2 CANNOT CLASSIFY. 2 is NOT collapsed into 1: it is
  # said aloud and falls through to arbitration exactly as before, so an
  # unreadable input can never silently pick either answer.
  #
  # The retry rides the EXISTING rc=3 contract (verdict.py verdict_from_rc:
  # 3 -> RETRY; cycle.py classify: retry -> RETRY; gate_full_cycle.sh's retry
  # branch), through the same path the coverage floor's exit 3 takes below: set
  # FAILED_PHASE and exit — the EXIT trap (gate_finish -> gate_write_verdict)
  # writes gate-verdict.json from the rc, so the file says RETRY, never PASS.
  # gate_full_cycle.sh greps the phrase `ceiling-killed shard(s) only` to bound
  # these retries at three (a box that cannot finish a shard must not loop
  # MAX_ATTEMPTS times); keep the wording in step with it.
  _ceil_dir=$(gate_shard_logdir "$S/gate_shard.log" "${GATE_SHARD_STAMP:-}")
  _ceil_args=(--shard-log "$S/gate_shard.log")
  if [ -n "$_ceil_dir" ]; then
    _ceil_args+=(--shard-logs-dir "$_ceil_dir")
  else
    echo "   ceiling-only classifier: per-shard logs not located (no KEPT line, or nothing newer than the launch stamp) — scanning the gate log alone"
  fi
  PYTHONPATH=scripts python3 -m gate.summary run-summary ceiling-only "$GATE_RUN_SUMMARY" "${_ceil_args[@]}" \
    2>"$S/gate_ceiling.err"; _ceil_rc=$?
  case "$_ceil_rc" in
    0) rec_ran shard_arbitration 3 "ceiling-killed only"
       echo "== gate: RETRY — sharded suite: ceiling-killed shard(s) only, no FAIL/crash${_ceil_dir:+ (per-shard logs: $_ceil_dir)} (see $S/gate_shard.log) =="
       FAILED_PHASE="sharded suite — ceiling-killed shard(s) only, no FAIL/crash (retry)"
       exit 3 ;;
    1) ;;  # not ceiling-only (a crash, a FAIL, an unexplained gap): arbitrate as before
    *) echo "   ceiling-only classifier: CANNOT CLASSIFY (rc=$_ceil_rc: $(tr '\n' ' ' <"$S/gate_ceiling.err" 2>/dev/null | cut -c1-240)) — arbitrating as before" ;;
  esac
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
  AOT_STORE_FAILS=$(grep -aoE 'store_failures=[0-9]+' "$S/gate_shard.log" 2>/dev/null \
               | grep -oE '[0-9]+$' | awk '{t+=$1} END{if(NR)print t}')
  if [ -n "${AOT_HITS:-}${AOT_MISSES:-}" ]; then
    echo "== gate: AOT cache: ${AOT_HITS:-?} hits / ${AOT_MISSES:-?} misses" \
         "(store failures: ${AOT_STORE_FAILS:-?}) =="
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
# Phase 4 (#3010): the coverage floor is the gate.coverage Python module.
# The CLI's EXIT CODE is the decision (0 ok / 3 retry — the same exit-3 retry
# contract gate_full_cycle.sh relies on). stdout carries the banner (echoed
# below, excluding the @cov: machine lines) plus machine lines this orchestrator
# harvests for telemetry + the trap's FAILED_PHASE. Python is AUTHORITATIVE; the
# dual-run bash re-enumerate that proved equivalence has been removed (the
# migration is complete). #3011 invariant: a missing/non-executable/empty-
# enumerate binary is a HARD fail that NEVER lowers the bar.
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
