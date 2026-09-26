#!/usr/bin/env bash
# pregate.sh — run everything a gate cycle checks that does not need the test
# suite, and print one verdict table. Exit 0 means "safe to launch a gate".
#
# WHY. A gate cycle is ~50 min and freezes every lane on the machine. Most gate
# FAILs that were not real regressions were knowable before launch: a RED
# ratchet, a corpus row that needed banking, a self-test that was never run by
# hand, a builder still running, a stale needle for a renamed diagnostic. Each of
# those used to be a separate step the coordinator had to remember. This script
# is the one step. It DERIVES the ratchet list from the gate (ratchet_sweep.sh),
# so it cannot drift from what the cycle runs.
#
# Usage:
#   scripts/pregate.sh                       # preflight + all build-free phases
#   scripts/pregate.sh --filters "a b c"     # + agent_check on both oracles
#   scripts/pregate.sh --base origin/main    # range for trailers/needles
#
# Phases (build-free ones run in parallel, each with its own log):
#   preflight   clean tree, HEAD contains BASE, no live gate, no builders,
#               fast-disk symlinks, /mnt/fast headroom
#   bezelc      build/bezelc is newer than every src/ file (corpus needs it)
#   ratchets    scripts/ratchet_sweep.sh (every check_*.py the gate runs)
#   selftests   gate self-tests + smart-push self-tests (markers redirected,
#               #3415: never touch a live gate's pidfile/donefile)
#   corpus      conformance-rules unittests, corpus.py check,
#               test_conformance.py, gate_rule_corpus_baseline.py --strict
#   needles     scripts/pregate_stale_needles.py (WARN only)
#   filters     optional: agent_check plain, then ASan (must print
#               "NATIVE SANITIZER: 0 check(s) skipped")
#
# It never builds except through --filters (which uses agent_check.sh), and it
# refuses to run while a gate is live, because the self-test phase must not
# race a running cycle.
set -uo pipefail

REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO" || exit 1

BASE="origin/main"
FILTERS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --base) BASE="$2"; shift 2 ;;
    --filters) FILTERS="$2"; shift 2 ;;
    -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
    *) echo "pregate: unknown argument '$1' (see --help)" >&2; exit 2 ;;
  esac
done

HEAD_SHA="$(git rev-parse --short HEAD)"
OUT="/var/tmp/bezel-scratch/pregate/${HEAD_SHA}-$(date +%H%M%S)"
mkdir -p "$OUT" || exit 1
FAST="${BEZEL_AOT_FAST_ROOT-/mnt/fast}"
PIDFILE="${BEZEL_GATE_PIDFILE:-/var/tmp/bezel-scratch/gate.pid}"

if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
  echo "pregate: a gate is running (pid $(cat "$PIDFILE")) — refusing." >&2
  exit 3
fi

declare -A RESULT NOTE
ORDER=()
record() { ORDER+=("$1"); RESULT[$1]="$2"; NOTE[$1]="${3:-}"; }

# --- preflight (serial, cheap) ------------------------------------------------
pf=PASS; pf_note=""
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  pf=FAIL; pf_note+="tracked changes uncommitted; "
fi
if [ -n "$(git ls-files --others --exclude-standard -- scripts/)" ]; then
  pf=FAIL; pf_note+="untracked files under scripts/ (#3413); "
fi
git fetch -q origin 2>/dev/null || pf_note+="(fetch failed, using cached $BASE) "
if ! git merge-base --is-ancestor "$BASE" HEAD 2>/dev/null; then
  pf=FAIL; pf_note+="HEAD does not contain $BASE; "
fi
if ! scripts/no_builders.sh > "$OUT/no_builders.log" 2>&1; then
  pf=FAIL; pf_note+="builders running (see no_builders.log); "
fi
if [ -n "$FAST" ] && mountpoint -q "$FAST" 2>/dev/null; then
  for d in aot-bincache aot-harness; do
    [ -L "/var/tmp/bezel-scratch/$d" ] || { pf=FAIL; pf_note+="/var/tmp/bezel-scratch/$d is not a symlink (run scripts/lane_setup.sh); "; }
  done
  use="$(df --output=pcent "$FAST" | tail -1 | tr -dc 0-9)"
  if [ "${use:-0}" -ge 93 ]; then pf=FAIL; pf_note+="$FAST at ${use}%; "
  elif [ "${use:-0}" -ge 88 ]; then pf_note+="$FAST at ${use}% (consider scripts/gc_scratch.sh --apply); "; fi
fi
ahead="$(git rev-list --count "$BASE"..HEAD)"
fixes="$(git log --format=%s "$BASE"..HEAD | grep -iE '^(fix|narrow)' | grep -oE '#[0-9]{3,5}' | sort -u | wc -l)"
record preflight "$pf" "${pf_note}${ahead} commits, ${fixes} distinct fix issues over $BASE"

# --- bezelc freshness --------------------------------------------------------
BEZELC="build/bezelc"
if [ ! -x "$BEZELC" ]; then
  record bezelc FAIL "no $BEZELC — build it (scripts/dev_heavy.sh build cmake --build build --target bezelc)"
elif [ -n "$(find src -newer "$BEZELC" -type f -print -quit)" ]; then
  record bezelc FAIL "$BEZELC is older than src/ — rebuild before trusting the corpus phase"
else
  record bezelc PASS "$BEZELC newer than every src/ file"
fi

# --- build-free phases, in parallel -----------------------------------------
run_bg() {  # name, command...
  local name="$1"; shift
  ( "$@" > "$OUT/$name.log" 2>&1; echo $? > "$OUT/$name.rc" ) &
}

run_bg ratchets scripts/ratchet_sweep.sh

selftests() {
  mkdir -p "$OUT/markers"
  BEZEL_GATE_PIDFILE="$OUT/markers/gate.pid" BEZEL_GATE_DONEFILE="$OUT/markers/gate.done" \
    python3 -m unittest discover -s scripts/gate/tests || return 1
  BEZEL_GATE_PIDFILE="$OUT/markers/gate.pid" BEZEL_GATE_DONEFILE="$OUT/markers/gate.done" \
    python3 -m unittest scripts.gate.tests.test_gate_and_push_smart_push || return 1
}
run_bg selftests selftests

corpus() {
  python3 -m unittest discover -s tests/conformance/rules || return 1
  python3 tests/conformance/rules/corpus.py check || return 1
  python3 -m unittest discover -s tests/conformance -p 'test_conformance.py' || return 1
  python3 scripts/gate_rule_corpus_baseline.py --self-test || return 1
  python3 scripts/gate_rule_corpus_baseline.py --strict --bezelc "$BEZELC" || return 1
}
if [ "${RESULT[bezelc]}" = PASS ]; then run_bg corpus corpus; fi

run_bg needles python3 scripts/pregate_stale_needles.py --base "$BASE"

wait

phase_result() {  # name, pass-note
  local name="$1" rc
  rc="$(cat "$OUT/$name.rc" 2>/dev/null || echo 99)"
  if [ "$rc" = 0 ]; then record "$name" PASS "$2"
  else record "$name" FAIL "rc=$rc — see $OUT/$name.log"; fi
}
phase_result ratchets "$(tail -1 "$OUT/ratchets.log")"
phase_result selftests "$(grep -E '^Ran ' "$OUT/selftests.log" | head -1)"
if [ "${RESULT[bezelc]}" = PASS ]; then
  phase_result corpus "$(grep -E '^gate_rule_corpus_baseline: OK' "$OUT/corpus.log" | tail -1 | cut -c1-90)"
else
  record corpus SKIP "needs a fresh bezelc"
fi
n_hits="$(grep -c '^  CANDIDATE' "$OUT/needles.log" 2>/dev/null)"
if [ "${n_hits:-0}" -gt 0 ]; then
  record needles WARN "$n_hits test(s) still name removed diagnostic text — check each (see needles.log)"
else
  record needles PASS "$(head -1 "$OUT/needles.log")"
fi

# --- optional targeted filters, both oracles (these DO build) ----------------
if [ -n "$FILTERS" ]; then
  # shellcheck disable=SC2086
  scripts/agent_check.sh $FILTERS > "$OUT/filters_plain.log" 2>&1
  prc=$?
  # shellcheck disable=SC2086
  BEZEL_AOT_FORCE_SANITIZE=address scripts/agent_check.sh $FILTERS > "$OUT/filters_asan.log" 2>&1
  arc=$?
  skipped_ok="$(grep -c 'NATIVE SANITIZER: 0 check(s) skipped' "$OUT/filters_asan.log")"
  if [ $prc -eq 0 ] && [ $arc -eq 0 ] && [ "${skipped_ok:-0}" -gt 0 ]; then
    record filters PASS "plain + ASan green, ASan leg ran"
  else
    record filters FAIL "plain rc=$prc asan rc=$arc asan-ran-markers=${skipped_ok:-0} — see $OUT/filters_*.log"
  fi
fi

# --- verdict -----------------------------------------------------------------
echo "pregate @ $HEAD_SHA (base $BASE) — logs in $OUT"
red=0
for k in "${ORDER[@]}"; do
  printf '  %-10s %-5s %s\n' "$k" "${RESULT[$k]}" "${NOTE[$k]}"
  [ "${RESULT[$k]}" = FAIL ] && red=$((red + 1))
done
if [ $red -eq 0 ]; then
  echo "pregate: READY — launch with: setsid nohup scripts/gate_and_push.sh > LOG 2>&1 < /dev/null &"
  exit 0
fi
echo "pregate: $red phase(s) FAILED — do NOT launch a gate"
exit 1
