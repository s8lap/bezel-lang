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

echo "== gate[1/*]: reconfigure (glob any new test files) =="
PHASE_T0=$(now)
"$DH" build cmake -S . -B build >"$S/gate_reconf.log" 2>&1; prc=$?
rec_ran configure "$prc"
[ "$prc" -ne 0 ] && { tail -15 "$S/gate_reconf.log"; fail "configure"; }

echo "== gate[2/*]: build bezel_tests =="
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
echo "== gate[2b/*]: lit diagnostic track (real bezelc + FileCheck, per-process) =="
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
echo "== gate[2c/*]: clang-tidy static-analysis (advisory; changed TUs only) =="
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

echo "== gate[3/*]: sharded suite (crash/undercount-checked) =="
PHASE_T0=$(now)
"$DH" suite tests/run_parallel.sh >"$S/gate_shard.log" 2>&1; rc=$?
rec_ran shard "$rc"
# Harvest run_parallel's own aggregate wall + shard count + per-shard times (obs only).
SHARD_COUNT=$(grep -oE 'across [0-9]+ shards' "$S/gate_shard.log" | grep -oE '[0-9]+' | tail -1); SHARD_COUNT=${SHARD_COUNT:-0}
SHARD_REPORTED_S=$(grep -oE 'across [0-9]+ shards, [0-9]+s' "$S/gate_shard.log" | grep -oE '[0-9]+s' | tr -d 's' | tail -1); SHARD_REPORTED_S=${SHARD_REPORTED_S:-0}
SHARD_TIMES=$(grep -oE '^shard [0-9]+: [0-9.]+s$' "$S/gate_shard.log" | sed -E 's/^shard [0-9]+: ([0-9.]+)s$/\1/' | paste -sd, -)
grep -E 'TOTAL:|## GATE FAILURE|died on|--- failing' "$S/gate_shard.log" || true
if [ "$rc" -ne 0 ]; then
  PHASE_T0=$(now)  # the flake-arbitration re-runs are their own (exceptional) phase
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
      if BEZEL_TEST_FILTER="$suite" timeout 180 build/tests/bezel_tests >/dev/null 2>&1; then passed_alone=1; break; fi
    done
    if [ "$passed_alone" -eq 1 ]; then
      echo "   FLAKY: $t — its suite PASSES alone within 5 tries (sharding pollution/timing, not a deterministic failure)"
    else
      echo "   REAL: $t — fails alone all 5 tries"; real=1
    fi
  done <<< "$suspects"
  [ "$real" -eq 1 ] && { rec_ran shard_arbitration "1" "$nsus suspects, a real fail"; fail "sharded suite — a suspect fails alone all 5 tries (real regression)"; }
  rec_ran shard_arbitration "0" "$nsus suspects, all flaky"
  echo "   all sharded suspects flaky (pass alone) -> continuing to serial (authoritative)"
fi

if [ "$MODE" != "--quick" ]; then
  echo "== gate[4/*]: serial suite (heap-order, catches sharded-hidden fails) =="
  # A flaky-crash test (rc=70 mid-run) aborts the serial binary with NO summary
  # line and NO '[ FAIL' — which the old grep passed silently. Require a COMPLETE
  # green summary; retry up to 3x if the run didn't complete (flake); hard-fail on
  # any assertion FAIL immediately, or on a persistent no-complete (real crash).
  PHASE_T0=$(now)
  serial_ok=0
  for attempt in 1 2 3; do
    SERIAL_ATTEMPTS=$attempt
    # timeout guards against a DEADLOCKED test (#1609 contention race) hanging the
    # serial binary forever; a hang leaves no clean summary -> treated as "did not
    # complete" -> retried, like a flaky crash.
    # 1500s (was 900): the suite grew past ~11.7k tests and the fork-per-test serial
    # run brushed 900s under producer-build contention (2026-07-12) -> spurious
    # "did not complete" retries. 1500s clears it at moderate load.
    timeout -k 30 1500 "$DH" suite build/tests/bezel_tests >"$S/gate_serial.log" 2>&1 || true
    if grep -qE '^\[ FAIL' "$S/gate_serial.log"; then
      rec_ran serial "1" "$attempt attempt(s), assertion FAIL"
      echo "   serial FAILs (attempt $attempt):"; grep -E '^\[ FAIL' "$S/gate_serial.log"; fail "serial suite — assertion failure"
    fi
    if grep -qE '[0-9]+ passed, 0 failed, [0-9]+ total' "$S/gate_serial.log"; then
      grep -E '[0-9]+ passed, [0-9]+ failed, [0-9]+ total' "$S/gate_serial.log" | tail -1
      serial_ok=1; break
    fi
    echo "   serial run did NOT complete cleanly (no green summary — likely a flaky-test crash mid-run); retry $attempt/3"
  done
  SERIAL_TOTAL=$(grep -oE '[0-9]+ passed, 0 failed, [0-9]+ total' "$S/gate_serial.log" | grep -oE '[0-9]+ total' | grep -oE '[0-9]+' | tail -1); SERIAL_TOTAL=${SERIAL_TOTAL:-0}
  [ "$serial_ok" -ne 1 ] && { rec_ran serial "1" "$SERIAL_ATTEMPTS attempts, no green summary"; fail "serial suite — did not complete green in 3 attempts (persistent crash, not a flake)"; }
  rec_ran serial "0" "$SERIAL_ATTEMPTS attempt(s)"
fi

if [ "$MODE" = "--full" ]; then
  echo "== gate[5/*]: asan_sweep (disposal/codegen/runtime) =="
  # via dev_heavy suite: the lock is re-entrant, so asan_sweep's inner dev_heavy
  # build does not deadlock; do NOT run asan_sweep bare + a build concurrently.
  PHASE_T0=$(now)
  "$DH" suite scripts/asan_sweep.sh >"$S/gate_asan.log" 2>&1; asan_rc=$?
  # asan_sweep exits 2 on configure/build failure. Previously `|| true` masked
  # that: a failed ASan build leaves no "ERROR: AddressSanitizer" markers, so the
  # error count below was 0 and the gate FALSE-PASSED having validated nothing.
  if [ "$asan_rc" -eq 2 ]; then
    rec_ran asan "$asan_rc" "ASan build failed"
    echo "   asan_sweep: build/configure FAILED (exit 2):"; tail -20 "$S/gate_asan.log"; fail "asan_sweep — ASan build failed (validated nothing)"
  fi
  # Count REAL sanitizer errors — the per-shard summary line is "shard N: X ASan
  # errors", so grepping the substring 'ASan error' false-counts the zero lines.
  # Sum the per-shard counts, and also catch the sanitizer's own error markers.
  errs=$(awk 'match($0,/ASan errors/){for(i=1;i<=NF;i++)if($i=="ASan"){s+=$(i-1)}} END{print s+0}' "$S/gate_asan.log")
  errs=$(( errs + $(grep -cE '(ERROR|SUMMARY): (Address|Leak)Sanitizer|runtime error:|detected memory leaks' "$S/gate_asan.log") ))
  shards_reported=$(grep -c 'ASan errors' "$S/gate_asan.log")
  ASAN_SHARDS=$shards_reported; ASAN_ERRS=$errs
  echo "   ASan shards reported: $shards_reported/8, real errors: $errs"
  # Enforce that all 8 shards actually ran — fewer means some silently skipped
  # (partial build failure / crash before reporting), which previously let the
  # gate pass having validated fewer than the full suite under ASan.
  if [ "$shards_reported" -ne 8 ]; then
    rec_ran asan "1" "$shards_reported/8 shards, incomplete"
    echo "   asan_sweep: only $shards_reported/8 shards reported (incomplete sweep):"; tail -20 "$S/gate_asan.log"; fail "asan_sweep — incomplete ($shards_reported/8 shards)"
  fi
  [ "$errs" -ne 0 ] && { rec_ran asan "1" "$errs errors"; grep -iE 'ERROR: AddressSanitizer|runtime error:|shard [0-9]+: [1-9]' "$S/gate_asan.log" | head; fail "asan_sweep"; }
  rec_ran asan "0" "$shards_reported/8 shards, $errs errors"
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
