"""scripts/gate/ — Python home for the gate/test-infra logic (#3010).

The gate/test-infra is ~3,555 lines of bash; the owner signed off on migrating
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
"""

__all__ = ["io", "summary", "verdict", "coverage", "arbitration"]
