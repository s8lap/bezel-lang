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
#   scripts/gate.sh            # DEFAULT: reconfigure + build + lit + tidy + ONE
#                              #   authoritative PLAIN sharded pass. NO serial.
#                              #   Fast + robust: plain compiles fit the per-shard
#                              #   timeout even cold and under multi-agent load.
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
set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "gate: not in a git repo" >&2; exit 2; }
MODE="${1:-default}"
S=/var/tmp/bezel-scratch; mkdir -p "$S"
DH=scripts/dev_heavy.sh

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
# (ARB_ENV) or an ASan-only failure would be falsely cleared as flaky.
ASAN=0
[ "$MODE" = "--full" ] && ASAN=1
case "${GATE_FAST_ASAN:-}" in
  1) ASAN=1 ;;
  0) ASAN=0 ;;
esac

SHARD_ENV=(); ARB_ENV=(); SHARD_NOTE=""; SHARD_LABEL="crash/undercount-checked"
if [ "$ASAN" -eq 1 ]; then
  # A COLD full-suite ASan compile is ~2-3x a plain compile per test, so the
  # run_parallel default 300s per-shard timeout (tuned for plain compiles: warm
  # ~45s, ~6x headroom) is too tight for the first ASan gate after any relink
  # (the AOT bincache keys on the test-binary hash, so a relink is a full cache
  # miss) — especially under multi-agent slice contention. Give the ASan pass the
  # same ~6x headroom over its warm ~137s => 900s (a genuine per-test hang is
  # still bounded by the harness's 120s compile / 30s run deadlines; this is only
  # the coarse per-shard backstop). A caller's explicit BEZEL_SHARD_TIMEOUT wins.
  SHARD_ENV=(env BEZEL_AOT_FORCE_SANITIZE=address BEZEL_SHARD_TIMEOUT="${BEZEL_SHARD_TIMEOUT:-900}")
  ARB_ENV=(env BEZEL_AOT_FORCE_SANITIZE=address)
  SHARD_NOTE="asan-instrumented"
  SHARD_LABEL="crash/undercount + generated-code ASan"
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
  printf '{"schema":1,"ts":"%s","epoch":%s,"host":"%s","mode":"%s","head":"%s","head_short":"%s","dirty":%s,"result":"%s","failed_phase":%s,"total_s":%s,"gtests":%s,"littests":%s,"shard_count":%s,"shard_reported_s":%s,"arbitration_suspects":%s,"serial_attempts":%s,"serial_total":%s,"asan_shards_reported":%s,"asan_errors":%s,"shard_times":[%s],"phases":[%s]}\n' \
    "$GATE_TS" "${GATE_EPOCH:-0}" "$GATE_HOST" "$MODE" "$GATE_HEAD" "$GATE_HEAD_SHORT" "$GATE_DIRTY" \
    "$result" "$fp" "$total" "${COV_GTESTS:-0}" "${COV_LITTESTS:-0}" "${SHARD_COUNT:-0}" "${SHARD_REPORTED_S:-0}" \
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
}

fail() { FAILED_PHASE="$1"; echo "== GATE FAIL: $1 =="; exit 1; }
# Emit summary+JSON on EVERY exit (pass or fail), preserving the exit code exactly.
trap 'rc=$?; gate_finish "$rc" || true; exit $rc' EXIT

echo "== gate[1/5]: reconfigure (glob any new test files) =="
PHASE_T0=$(now)
"$DH" build cmake -S . -B build >"$S/gate_reconf.log" 2>&1; prc=$?
rec_ran configure "$prc"
[ "$prc" -ne 0 ] && { tail -15 "$S/gate_reconf.log"; fail "configure"; }

echo "== gate[2/5]: build bezel_tests =="
PHASE_T0=$(now)
"$DH" build cmake --build build --target bezel_tests -j16 >"$S/gate_build.log" 2>&1; prc=$?
rec_ran build "$prc"
[ "$prc" -ne 0 ] && { grep -iE 'error:|FAILED:' "$S/gate_build.log" | head -20; fail "build"; }
echo "   binary: $(ls -la --time-style=+%H:%M:%S build/tests/bezel_tests | awk '{print $6}')"

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
echo "== gate[4/5]: clang-tidy static-analysis (advisory; changed TUs only) =="
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

echo "== gate[5/5]: sharded suite ($SHARD_LABEL) — the authoritative oracle =="
PHASE_T0=$(now)
"$DH" suite "${SHARD_ENV[@]}" tests/run_parallel.sh >"$S/gate_shard.log" 2>&1; rc=$?
# When ASan is on (--full), the shard phase's recorded duration INCLUDES the
# ASan compile+run (SHARD_NOTE marks it), so the ASan cost is captured in the
# phases array here — there is no separate asan phase.
rec_ran shard "$rc" "$SHARD_NOTE"
# Harvest run_parallel's own aggregate wall + shard count + per-shard times (obs only).
SHARD_COUNT=$(grep -oE 'across [0-9]+ shards' "$S/gate_shard.log" | grep -oE '[0-9]+' | tail -1); SHARD_COUNT=${SHARD_COUNT:-0}
SHARD_REPORTED_S=$(grep -oE 'across [0-9]+ shards, [0-9]+s' "$S/gate_shard.log" | grep -oE '[0-9]+s' | tr -d 's' | tail -1); SHARD_REPORTED_S=${SHARD_REPORTED_S:-0}
SHARD_TIMES=$(grep -oE '^shard [0-9]+: [0-9.]+s$' "$S/gate_shard.log" | sed -E 's/^shard [0-9]+: ([0-9.]+)s$/\1/' | paste -sd, -)
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
  # Collect ALL suspects: shard CRASHES (died on: a program killed its host) AND
  # assertion FAILs. BOTH can be flaky under sharding — a crash, or a FAIL from
  # test-pollution (a polluter leaves global state; the round-robin lands it with
  # a snapshot/leak test). ARBITRATE uniformly: re-run each suspect's SUITE alone
  # up to 5x. A DETERMINISTIC failure (real regression / over-rejection) fails all
  # 5; a pollution/timing flake passes at least once. One run isn't enough — it
  # shares the flake rate. With serial gone (#1861) THIS is the authoritative
  # re-check: a suspect that passes alone within 5 tries is a sharding flake and
  # the gate continues; a suspect that fails all 5 is a real regression and fails.
  crashed=$(grep 'died on:' "$S/gate_shard.log" | sed -E 's/.*died on: //')
  failed=$(grep -E '^\[ FAIL' "$S/gate_shard.log" | sed -E 's/^\[ FAIL \] //' | awk '{print $1}')
  suspects=$(printf '%s\n%s\n' "$crashed" "$failed" | grep -v '^$' | sort -u)
  [ -z "$suspects" ] && { rec_ran shard_arbitration "1" "no crash/FAIL marker"; fail "sharded suite — nonzero exit, no crash/FAIL marker (see $S/gate_shard.log)"; }
  nsus=$(printf '%s\n' "$suspects" | grep -c .)
  ARB_SUSPECTS=$nsus
  # >8 distinct failing suites is a real regression, not pollution flakes; fail
  # fast (and bound the arbitration cost).
  if [ "$nsus" -gt 8 ]; then
    grep -E '^\[ FAIL' "$S/gate_shard.log" | head; rec_ran shard_arbitration "1" "$nsus suspects"; fail "sharded suite — $nsus failing tests (real regression, too many to be flakes)"
  fi
  real=0
  while IFS= read -r t; do
    [ -z "$t" ] && continue
    suite="${t%%.*}"
    passed_alone=0
    for k in 1 2 3 4 5; do
      # ARB_ENV forces ASan on the re-run in the default (ASan) mode so an
      # ASan-only failure reproduces (empty array in --no-asan -> identical command).
      if BEZEL_TEST_FILTER="$suite" "${ARB_ENV[@]}" timeout 180 build/tests/bezel_tests >/dev/null 2>&1; then passed_alone=1; break; fi
    done
    if [ "$passed_alone" -eq 1 ]; then
      echo "   FLAKY: $t — its suite PASSES alone within 5 tries (sharding pollution/timing, not a deterministic failure)"
    else
      echo "   REAL: $t — fails alone all 5 tries"; real=1
    fi
  done <<< "$suspects"
  [ "$real" -eq 1 ] && { rec_ran shard_arbitration "1" "$nsus suspects, a real fail"; fail "sharded suite — a suspect fails alone all 5 tries (real regression)"; }
  rec_ran shard_arbitration "0" "$nsus suspects, all flaky"
  echo "   all sharded suspects flaky (pass alone within 5 tries) -> ACCEPT (shard-authoritative; no serial backstop)"
fi

# Combined coverage: the gtest behavior track and the lit diagnostic track are
# separate runners, so neither alone reports the real total. Sum them here (this
# is the one place that ran both). Best-effort — never fails the gate.
gtests=$(grep -oE 'TOTAL: [0-9]+ passed' "$S/gate_shard.log" 2>/dev/null | grep -oE '[0-9]+' | tail -1)
littests=$(grep -oE 'Passed: *[0-9]+' "$S/gate_lit.log" 2>/dev/null | grep -oE '[0-9]+' | tail -1)
COV_GTESTS=${gtests:-0}; COV_LITTESTS=${littests:-0}
if [ -n "${gtests:-}" ]; then
  echo "== gate: coverage: ${gtests} gtest + ${littests:-0} lit = $(( gtests + ${littests:-0} )) tests passing =="
fi

echo "== GATE PASS =="
