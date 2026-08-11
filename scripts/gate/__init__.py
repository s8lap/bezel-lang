"""scripts/gate/ — Python home for the gate/test-infra logic (#3010).

The gate/test-infra is ~3,555 lines of bash; migrating
the LOGIC-heavy parts to Python (stdlib only — `unittest`, no pytest, no new
deps). This package is the migration target. It is built up phase by phase with
an emit-alongside -> prove-equivalence -> switch methodology: nothing switches
until the new module is shown byte-identical to the old script over the real
historical `gate-timings.jsonl`.

Phase 0 (this package + `io.py`) lands no logic that a gate consumes yet; it is
the scaffold every later phase composes on.

Phase 2 (`summary.py`, #3012) reads the per-shard JSON summaries the C++
harness emits (BEZEL_SHARD_SUMMARY) and compares them against the legacy
stderr summary line — observation-only equivalence data for the eventual
reader switch (Phase 2c).

Phase 3 (`verdict.py`, #3002) writes an authoritative `gate-verdict.json` on
every gate exit — ONE atomically-written file that fixes the exit-code-trap
class (a backgrounded wrapper's `$?` is not the gate's). ADDITIVE: no consumer
reads it yet (Phase 3c switches the readers after a prove-out).

Phase 4a (`coverage.py`, #3002 / #3011) ports the coverage floor as a
unit-tested Python module. FOUNDATION ONLY: written + standalone-validated,
NOT yet wired into gate.sh (Wave 2 replaces the bash block at gate.sh:882-973
with a call here). Preserves the bash invariants (missing-binary=>hard-fail
#3011, enumerate==0=>hard-fail, zero-rows=>hard-fail, ran>=expected superset
OK) — pinned by test_coverage.py.

Phase 4b (`arbitration.py`, #1861 / #2100) ports the flake arbitration
(gate.sh:432-524) as a unit-tested Python module. FOUNDATION ONLY: not wired
into gate.sh yet (Wave 2 substitutes the call). Ports arbitrate() +
arb_run_suite() and the 2c legacy-grep site at :451 in one move (the
`^[0-9]+ passed, [0-9]+ failed` harvest moves into _parse_summary). The five
invariants (ran-nothing-is-not-a-pass, [FAIL]-never-sampled, `<none>`-strip,
the 0/1/2 return contract never collapsed, ARB_ENV forces ASan) are each
pinned by test_arbitration.py.

Wave 3 (`telemetry.py`, #3010) ports the schema:2 gate-timings.jsonl record
builder (gate.sh emit_json). The 25-field bash printf, the awk per-binary
merge (with its `exp`/`ran` gawk-builtin-names footgun), and the j_* null-vs-0
conditionals become a dict + json.dumps + dict union + None-vs-0. Proven
byte-identical to the old bash over a controlled dual-run (8 cases, every
null-vs-0 branch, zero diff); gate.sh now calls `python3 -m gate.telemetry
emit`. The schema:2 field set/order and the null-vs-0 distinction are pinned
by test_telemetry.py (the blocking gate.trend R1-R5 depend on them).

Wave 3 (`cycle.py`, #3010) consolidates the recurring verdict-classification
pattern (read a verdict/signal -> classify accept/retry/bail ->
conservative-on-disagreement) that was inlined across FOUR runner-script sites
(gate_full_cycle's run_until_trustworthy + ASan-ran check, gate_and_push's
verdict-before-push, gate_wait's classifier). classify maps a verdict value to
a Decision; cross_check reconciles two derivations WITHOUT EVER accepting on
disagreement (the false-accept-prevention that stopped the
55-truncated-gates-over-24-days class); assert_asan_ran is the ASan-evidence
check. The four sites dual-consume (legacy bash + gate.cycle), assert
0-disagreement, then switch to the Python. Pinned by test_cycle.py's truth
tables (classify 5x2, cross_check 3x3) + the 0-disagreement proof.
"""

__all__ = ["io", "summary", "verdict", "coverage", "arbitration", "telemetry",
           "cycle", "canary"]
