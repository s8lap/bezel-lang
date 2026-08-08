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
"""

__all__ = ["io", "summary", "verdict"]
