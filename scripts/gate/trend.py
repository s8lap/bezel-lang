#!/usr/bin/env python3
"""gate.trend — structural rules over gate-timings.jsonl (#3010).

WHY THIS EXISTS (#3010)
-----------------------
gate.sh has been appending one JSON record per gate since 2026-07-14. As of
2026-08-08 that file held 465 records over 24 days and had ZERO readers:
`grep -rn gate-timings scripts/ tests/ CLAUDE.md CMakeLists.txt` returned only
gate.sh's own writes.

In those 465 records the gate printed `pass` 55 times while its own coverage
number said the run was truncated:

    pass & gtests == 0                  26 gates    9.21 h
    pass & gtests < 99% of high-water   29 gates    6.19 h
    ---------------------------------------------------------
                                         55 gates   15.40 h

#3002 — a GATE PASS over a run that executed 5,156 of 16,482 tests — was legible
in this file, in plain numbers, for days before anyone noticed it by other
means. Its whole class was legible for weeks. The data was not missing.
Nothing read it.

So this module is deliberately NOT a dashboard. It is a set of structural rules
that fail the gate, wired into gate_and_push.sh's push path.

STRUCTURAL RULES ONLY — NO TUNED THRESHOLDS
-------------------------------------------
Every rule below is a CONTRADICTION, not a slow-down: a claim the record makes
that another field in the same record (or the run's own history) refutes. A
tuned threshold ("shard_s > 1.5x median") fires on ~20% of records and is
ignored within a day; a contradiction fires only when something is actually
wrong, so it can be allowed to fail the gate.

SELF-CHECK (the point of #3000)
-------------------------------
This module ALWAYS prints its "trend: N records scanned, M rules evaluated"
line. A checker that silently no-ops is #3000 applied to the fix for #3000 —
gate.sh's cold/warm predicate read 0 unconditionally for weeks because nothing
ever asserted it had looked at anything. Callers must treat a missing scan line
as a failure, not as a pass.

EXIT CODES
  0  no rule fired
  1  at least one rule fired (details on stdout), OR the newest record is
     malformed JSON (there is nothing to judge — refusing to silently grade an
     older record instead)
  2  could not read the log at all (nothing was checked — NOT a pass)
 64  unknown CLI argument

PROVENANCE
Lifted verbatim from the gate_trend.sh inline heredoc (2026-08-08). The five
rules and their semantics are a PRESERVATION CONTRACT (Plan-A 1.16/1.17/1.18);
they were ported, not redesigned. The equivalence proof (old heredoc vs this
module, zero-diff over the 465+ historical records) lives in the Phase-1 commit
message. `gate_trend.sh` is now a thin wrapper: `exec python3 -m gate.trend`.
"""

import json
import os
import sys

RULES = [
    "R1 coverage-short-but-pass",
    "R2 binary-contributed-zero",
    "R3 warm-slower-than-cold",
    "R4 unmeasured-sentinel",
    "R5 coverage-floor-disabled",
]

DEFAULT_TLOG = "/var/tmp/bezel-scratch/gate-timings/gate-timings.jsonl"


def read_trend_log(path):
    """Read the jsonl log the way the rules need it.

    Returns (records, malformed_count, last_line_bad). `last_line_bad` is True
    iff the newest non-blank line failed to parse — exactly the case the #3010
    refusal exists for: under `--last`, silently judging an OLDER record would
    answer a question about a DIFFERENT gate (and, on the historical log, very
    likely fire R1 on a weeks-old run). The malformed-newest line is therefore
    a loud FAIL, handled in main(), not a silent fallback.

    This is deliberately NOT io.read_jsonl: that helper skips malformed lines
    silently (right for a general manifest); this one counts them and flags the
    newest, because here a malformed line IS the signal.
    """
    records = []
    malformed = 0
    last_line_bad = False
    with open(path, errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
                last_line_bad = False
            except Exception:
                malformed += 1
                last_line_bad = True
    return records, malformed, last_line_bad


def judge(records, only_last=False, malformed=0):
    """Apply the five structural rules (R1-R5) to `records`. PURE: no I/O.

    Returns (scan_line, findings):
      * scan_line — the unconditional "trend: N records scanned, M rules
        evaluated" string (the caller's proof this ran; a missing scan line is
        a failure, not a pass — Plan-A 1.17).
      * findings  — list of (severity, rule, record_index, message, rank).

    `records` is the ALREADY-LIMITED list (limit/--last window applied by the
    caller); the high-water and cold histories are built over this same window,
    matching the heredoc (a rule never uses information from outside the window,
    and never from the future within it).

    R1 coverage-short-but-pass: pass + gtests==0, or pass + gtests < 99% of
        expected (expected_gtests for new records; the running high-water as a
        proxy for schema:1 records that predate expected_gtests).
    R2 binary-contributed-zero: a test binary with expected>0 ran 0 (FAIL) or
        ran<expected (WARN).
    R3 warm-slower-than-cold: a warm shard pass slower than the worst of the
        last <=5 cold passes for the same mode — a warm cache cannot be slower
        than a cold one (#3003 signature). Under --last the cold history is
        empty, so R3 cannot fire (faithful to the heredoc).
    R4 unmeasured-sentinel: a passing gate whose shard phase ran >30s but gtests
        is null/0 (FAIL), or whose AOT cache stats / cache_n are absent after a
        ran shard (WARN). Scoped to PASS — a gate that failed early legitimately
        never reaches the coverage harvest.
    R5 coverage-floor-disabled: result == pass and (coverage_floor_enforced is
        not True or coverage_floor_satisfied is False) — a PASS record for which
        the floor was never invoked, or was invoked and did not report
        satisfied (per #3737; a floor FAIL on a result=fail record is not this
        rule's concern — `result` already conveys it).
    """
    # High-water of gtests, used as the expected-count proxy for schema:1 records
    # written before gate.sh recorded `expected_gtests` (#3002 landed 2026-08-07).
    # A NEW record carries expected_gtests and is judged against that directly.
    hw = 0
    hw_at = []
    for r in records:
        hw_at.append(hw)
        g = r.get("gtests") or 0
        if g > hw:
            hw = g

    findings = []  # (severity, rule, record_index, message, rank)

    def ts(r):   return r.get("ts", "?")
    def hd(r):   return r.get("head_short", "?")
    def mode(r): return r.get("mode", "?")

    # Cold-run baselines per mode, for R3. Built as we walk forward so a rule
    # never uses information from the future.
    cold_hist = {}

    # only_last on an EMPTY log would yield range(-1, 0) -> records[-1] ->
    # IndexError (a crash, not the exit-2 "nothing checked" the caller expects).
    # Guard: take the last index only when there IS a last record; otherwise
    # iterate the (empty) full range and let the scan line report 0 scanned.
    indices = range(len(records) - 1, len(records)) if (only_last and records) else range(len(records))

    for i in indices:
        r = records[i]
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
        # Scope R4 to a PASSING gate. A gate that failed early (e.g. the sharded
        # pass exited nonzero) legitimately never reaches the coverage harvest, so
        # `gtests` is unmeasured for a good reason. Firing there would make every
        # real failure carry a second, spurious finding — and a rule that cries wolf
        # on failures is a rule people learn to skip past.
        sh = phases.get("shard")
        if result == "pass" and sh and sh.get("status") == "ran" and (sh.get("s") or 0) > 30:
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

        # ---- R5: a PASS without the floor's blessing -------------------------
        # fix #3737: `coverage_floor_enforced` used to be VALUE-based (true only
        # when the floor's own `ran >= expected` comparison passed), and this rule
        # fired on `floor is False` unscoped to result — so a record whose floor
        # genuinely ran, found a shortfall, and correctly reported result=fail was
        # ALSO flagged "the gate ran without the #3002 floor". The field is now
        # split: `coverage_floor_enforced` is presence-based (True iff gate.sh
        # invoked the floor; None iff it never reached that code) and
        # `coverage_floor_satisfied` keeps the old value (did the comparison pass).
        #
        # R5 fires on a PASS that either never invoked the floor OR invoked it and
        # was not satisfied. The second arm is load-bearing, not redundant with
        # `result`: gate.sh defaults GATE_FLOOR_OK to 0 when coverage.py exits 0
        # without printing `@cov:floor_ok=`, so a PASS record with
        # satisfied=false is reachable, and it means the pass carries no evidence
        # the floor agreed. Scoped to PASS like R4, for R4's reason: a fail/retry
        # that stopped before the floor legitimately never invokes it, and a
        # coverage-short fail is already fully conveyed by result=fail.
        satisfied = r.get("coverage_floor_satisfied")
        if result == "pass" and (floor is not True or satisfied is False):
            if floor is not True:
                why = (f"coverage_floor_enforced={floor!r} — the gate PASSED "
                       f"without ever invoking the #3002 floor")
            else:
                why = ("coverage_floor_satisfied=False — the gate PASSED although "
                       "the #3002 floor did not report satisfied")
            findings.append(("FAIL", RULES[4], i,
                f"{ts(r)} {hd(r)}: {why}; its pass is not an acceptance", 999.0))

    scan_line = (f"trend: {len(records)} records scanned, {len(RULES)} rules evaluated"
                 + (f", {malformed} malformed lines skipped" if malformed else ""))
    return scan_line, findings


def format_output(scan_line, findings, n_records, show):
    """Format scan_line + the per-rule findings block + final summary line.

    Returns (lines, exit_code). `lines` is the exact sequence main() prints
    (each element is one print() call, mirroring the heredoc's per-print output
    so the byte stream is identical). Worst-first WITHIN a rule (stable sort on
    rank, so equal ranks keep chronological insertion order); rules in RULES
    order across the block — the diagnostic value is in the extreme cases, and
    chronological truncation hid the 31%-coverage GATE PASS (#3002) behind
    twelve older records on this script's first run.
    """
    lines = [scan_line]
    fails = [f for f in findings if f[0] == "FAIL"]
    warns = [f for f in findings if f[0] == "WARN"]

    if not findings:
        lines.append("trend: no rule fired.")
        return lines, 0

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
        lines.append(f"\n## {rule}: {len(hits)} record(s), {nf} FAIL  (worst first)")
        for rank, sev, msg in hits[:show]:
            lines.append(f"   [{sev}] {msg}")
        if len(hits) > show:
            lines.append(f"   ... and {len(hits)-show} more (BEZEL_GATE_TREND_SHOW=N to see more)")

    lines.append(f"\ntrend: {len(fails)} FAIL, {len(warns)} WARN across {n_records} records")
    return lines, (1 if fails else 0)


def main(argv):
    """CLI entry. Returns the exit code (0/1/2/64). See module docstring."""
    tlog = os.environ.get("BEZEL_GATE_TLOG", DEFAULT_TLOG)
    limit = int(os.environ.get("BEZEL_GATE_TREND_LIMIT", "0"))  # 0 = all records
    only_last = False                                          # --last: newest only

    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--log":
            if i + 1 >= len(argv):
                sys.stderr.write("gate_trend: --log requires a value\n")
                return 64
            tlog = argv[i + 1]; i += 2
        elif a == "--limit":
            if i + 1 >= len(argv):
                sys.stderr.write("gate_trend: --limit requires a value\n")
                return 64
            limit = int(argv[i + 1]); i += 2
        elif a == "--last":
            only_last = True; i += 1
        elif a in ("-h", "--help"):
            # The old script printed its own header rationale (sed 2,40p); this
            # module carries that same rationale as its docstring.
            print(__doc__)
            return 0
        else:
            print(f"gate_trend: unknown argument: {a}", file=sys.stderr)
            return 64

    if not os.access(tlog, os.R_OK):
        # `-r` test: a missing file is also unreadable, which is what we want —
        # "no check ran" (exit 2) must stay distinct from "checked, all green".
        print(f"trend: CANNOT READ {tlog} — 0 records scanned, 0 rules evaluated")
        print("trend: this is NOT a pass; nothing was checked.")
        return 2

    records, malformed, last_line_bad = read_trend_log(tlog)

    # If the NEWEST line is unparseable, --last would silently fall back to
    # judging an OLDER record — quietly answering a question about a different
    # gate, and (given the historical log) very likely firing R1 on a run from
    # weeks ago. That is the failure mode this whole module exists to stop, so
    # it is loud instead. (This short-circuits BEFORE judging, so its scan line
    # reports 0 rules evaluated and no malformed suffix — matching the heredoc.)
    if last_line_bad:
        print(f"trend: {len(records)} records scanned, 0 rules evaluated")
        print("trend: FAIL — the NEWEST record is malformed JSON. The gate that just ran "
              "did not record a readable result, so there is nothing to judge. "
              "(Refusing to silently grade an older record instead.)")
        return 1

    if limit > 0:
        records = records[-limit:]

    scan_line, findings = judge(records, only_last=only_last, malformed=malformed)
    show = int(os.environ.get("BEZEL_GATE_TREND_SHOW", "12"))
    lines, rc = format_output(scan_line, findings, len(records), show)
    for line in lines:
        print(line)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
