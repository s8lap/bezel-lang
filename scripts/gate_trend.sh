#!/usr/bin/env bash
# gate_trend.sh — read gate-timings.jsonl back and SHOUT when a gate's own
# telemetry contradicts its verdict.
#
# WHY THIS EXISTS (#3010)
# ----------------------
# gate.sh has been appending one JSON record per gate since 2026-07-14. As of
# 2026-08-08 that file held 465 records over 24 days and had ZERO readers:
# `grep -rn gate-timings scripts/ tests/ CLAUDE.md CMakeLists.txt` returned only
# gate.sh's own writes.
#
# In those 465 records the gate printed `pass` 55 times while its own coverage
# number said the run was truncated:
#
#     pass & gtests == 0                  26 gates    9.21 h
#     pass & gtests < 99% of high-water   29 gates    6.19 h
#     ---------------------------------------------------------
#                                         55 gates   15.40 h
#
# #3002 — a GATE PASS over a run that executed 5,156 of 16,482 tests — was
# legible in this file, in plain numbers, for days before anyone noticed it by
# other means. Its whole class was legible for weeks. The data was not missing.
# Nothing read it.
#
# So this script is deliberately NOT a dashboard. It is a set of structural
# rules that fail the gate, wired into gate_and_push.sh's push path.
#
# STRUCTURAL RULES ONLY — NO TUNED THRESHOLDS
# -------------------------------------------
# Every rule below is a CONTRADICTION, not a slow-down: a claim the record makes
# that another field in the same record (or the run's own history) refutes. A
# tuned threshold ("shard_s > 1.5x median") fires on ~20% of records and is
# ignored within a day; a contradiction fires only when something is actually
# wrong, so it can be allowed to fail the gate.
#
# SELF-CHECK (the point of #3000)
# -------------------------------
# This script ALWAYS prints its "trend: N records scanned, M rules evaluated"
# line. A checker that silently no-ops is #3000 applied to the fix for #3000 —
# gate.sh's cold/warm predicate read 0 unconditionally for weeks because nothing
# ever asserted it had looked at anything. Callers must treat a missing scan
# line as a failure, not as a pass.
#
# EXIT CODES
#   0  no rule fired
#   1  at least one rule fired (details on stdout)
#   2  could not read the log at all (nothing was checked — NOT a pass)
set -uo pipefail

TLOG="${BEZEL_GATE_TLOG:-/var/tmp/bezel-scratch/gate-timings/gate-timings.jsonl}"
LIMIT="${BEZEL_GATE_TREND_LIMIT:-0}"     # 0 = all records
ONLY_LAST=0                              # --last: judge only the newest record

while [ $# -gt 0 ]; do
  case "$1" in
    --log)   TLOG="$2"; shift 2 ;;
    --limit) LIMIT="$2"; shift 2 ;;
    --last)  ONLY_LAST=1; shift ;;
    -h|--help)
      sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "gate_trend: unknown argument: $1" >&2; exit 64 ;;
  esac
done

if [ ! -r "$TLOG" ]; then
  echo "trend: CANNOT READ $TLOG — 0 records scanned, 0 rules evaluated"
  echo "trend: this is NOT a pass; nothing was checked."
  exit 2
fi

python3 - "$TLOG" "$LIMIT" "$ONLY_LAST" <<'PY'
import json, os, sys

path, limit, only_last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])

recs = []
malformed = 0
for line in open(path, errors="replace"):
    line = line.strip()
    if not line:
        continue
    try:
        recs.append(json.loads(line))
    except Exception:
        malformed += 1

if limit > 0:
    recs = recs[-limit:]

# --- history needed by the rules -------------------------------------------
# High-water of gtests, used as the expected-count proxy for schema:1 records
# written before gate.sh recorded `expected_gtests` (#3002 landed 2026-08-07).
# A NEW record carries expected_gtests and is judged against that directly.
hw = 0
hw_at = []
for r in recs:
    hw_at.append(hw)
    g = r.get("gtests") or 0
    if g > hw:
        hw = g

findings = []          # (severity, rule, record_index, message)
RULES = ["R1 coverage-short-but-pass", "R2 binary-contributed-zero",
         "R3 warm-slower-than-cold",   "R4 unmeasured-sentinel",
         "R5 coverage-floor-disabled"]

def ts(r):  return r.get("ts", "?")
def hd(r):  return r.get("head_short", "?")
def mode(r):return r.get("mode", "?")

# Cold-run baselines per mode, for R3. Built as we walk forward so a rule never
# uses information from the future.
cold_hist = {}

judge = range(len(recs) - 1, len(recs)) if only_last else range(len(recs))

for i in judge:
    r = recs[i]
    result   = r.get("result")
    gtests   = r.get("gtests")
    expected = r.get("expected_gtests")        # None on schema:1
    cold     = r.get("cold")                   # None on schema:1
    floor    = r.get("coverage_floor_enforced")
    per_bin  = r.get("binaries") or {}
    shard_s  = r.get("shard_reported_s") or 0

    # ---- R1: the verdict contradicts the coverage the run itself reported ---
    if result == "pass":
        want = expected if isinstance(expected, int) and expected > 0 else None
        if want is None:
            # legacy proxy: the suite only grows, so a pass far below the
            # running high-water means tests that existed did not run.
            want = hw_at[i]
            proxy = " (high-water proxy; record predates expected_gtests)"
        else:
            proxy = ""
        got = gtests if isinstance(gtests, int) else 0
        if want and got == 0:
            findings.append(("FAIL", RULES[0], i,
                f"{ts(r)} {hd(r)} mode={mode(r)}: result=pass but gtests=0 "
                f"(expected ~{want}){proxy} — the run reported NO tests and passed",
                0.0))
        elif want and got < want * 0.99:
            findings.append(("FAIL", RULES[0], i,
                f"{ts(r)} {hd(r)} mode={mode(r)}: result=pass but only {got} of "
                f"{want} gtests ran ({100.0*got/want:.1f}%){proxy}",
                100.0 * got / want))

    # ---- R2: a whole test binary contributed nothing --------------------
    # The 5,156-of-16,482 signature: 16482 - 11326 = 5156, i.e. the codegen
    # binary contributed exactly zero while every other binary reported.
    for name, v in sorted(per_bin.items()):
        try:
            ran, exp = int(v.get("ran", 0)), int(v.get("expected", 0))
        except Exception:
            continue
        if exp > 0 and ran == 0:
            findings.append(("FAIL", RULES[1], i,
                f"{ts(r)} {hd(r)}: binary '{name}' ran 0 of {exp} tests "
                f"— a whole area contributed nothing", 999.0))
        elif exp > 0 and ran < exp:
            findings.append(("WARN", RULES[1], i,
                f"{ts(r)} {hd(r)}: binary '{name}' ran {ran} of {exp}", 999.0))

    # ---- R3: a WARM run slower than the worst recent COLD run ------------
    # Not a threshold: a warm cache cannot legitimately be slower than cold.
    # This is the #3003 signature (the bincache went write-only, so the "warm"
    # ASan pass took 791s against 568-644s for every cold one).
    if cold is False and shard_s:
        base = cold_hist.get(mode(r), [])[-5:]
        if base and shard_s > max(base):
            findings.append(("FAIL", RULES[2], i,
                f"{ts(r)} {hd(r)} mode={mode(r)}: WARM shard pass took {shard_s}s, "
                f"slower than the worst of the last {len(base)} COLD runs "
                f"({max(base)}s) — a warm cache cannot be slower than a cold one; "
                f"suspect the cache stopped storing (see #3003)", 999.0))
    if cold is True and shard_s:
        cold_hist.setdefault(mode(r), []).append(shard_s)

    # ---- R4: sentinel — a guard reported a number it cannot have measured -
    # `"gtests": 0` on a passing run is how 26 records came to exist. Any field
    # that is 0/null WHILE ITS PHASE RAN is a measurement that did not happen,
    # and must never be silently indistinguishable from a real zero.
    phases = {p.get("name"): p for p in (r.get("phases") or [])}
    sh = phases.get("shard")
    if sh and sh.get("status") == "ran" and (sh.get("s") or 0) > 30:
        if not gtests:
            findings.append(("FAIL", RULES[3], i,
                f"{ts(r)} {hd(r)}: shard phase ran {sh.get('s')}s but gtests is "
                f"{gtests!r} — the count was never measured, not measured as zero", 999.0))
        aot = r.get("aot") or {}
        if aot and (aot.get("hits") is None or aot.get("misses") is None):
            findings.append(("WARN", RULES[3], i,
                f"{ts(r)} {hd(r)}: AOT cache stats absent after a "
                f"{sh.get('s')}s shard phase", 999.0))
        if r.get("cache_n") == 0 and cold is not None:
            findings.append(("WARN", RULES[3], i,
                f"{ts(r)} {hd(r)}: cache_n=0 — the store was read as empty; if it "
                f"is not, the read is broken (this is the #3000 shape)", 999.0))

    # ---- R5: the coverage floor disabled itself --------------------------
    if floor is False:
        findings.append(("FAIL", RULES[4], i,
            f"{ts(r)} {hd(r)}: coverage_floor_enforced=false — the gate ran "
            f"without the #3002 floor; its pass is not an acceptance", 999.0))

# --- output ---------------------------------------------------------------
# The scan line is UNCONDITIONAL and is the caller's proof that this ran.
print(f"trend: {len(recs)} records scanned, {len(RULES)} rules evaluated"
      + (f", {malformed} malformed lines skipped" if malformed else ""))

SHOW = int(os.environ.get("BEZEL_GATE_TREND_SHOW", "12"))
fails = [f for f in findings if f[0] == "FAIL"]
warns = [f for f in findings if f[0] == "WARN"]

if not findings:
    print("trend: no rule fired.")
    sys.exit(0)

by_rule = {}
for sev, rule, i, msg, rank in findings:
    by_rule.setdefault(rule, []).append((rank, sev, msg))

for rule in RULES:
    hits = by_rule.get(rule)
    if not hits:
        continue
    nf = sum(1 for _, s, _ in hits if s == "FAIL")
    # WORST FIRST, not chronological: the diagnostic value is in the extreme
    # cases. Chronological truncation hid the 31%-coverage GATE PASS (#3002)
    # behind twelve older records on the first run of this script.
    hits.sort(key=lambda h: h[0])
    print(f"\n## {rule}: {len(hits)} record(s), {nf} FAIL  (worst first)")
    for rank, sev, msg in hits[:SHOW]:
        print(f"   [{sev}] {msg}")
    if len(hits) > SHOW:
        print(f"   ... and {len(hits)-SHOW} more (BEZEL_GATE_TREND_SHOW=N to see more)")

print(f"\ntrend: {len(fails)} FAIL, {len(warns)} WARN across {len(recs)} records")
sys.exit(1 if fails else 0)
PY
