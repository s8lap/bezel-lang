#!/usr/bin/env bash
# Conformance harness runner — thin wrapper over the Python implementation.
#
# Usage:  tests/conformance/run_conformance.sh <bezelc-binary>
#
# The wrap heuristics, per-case accounting, crash classification, and
# expectations were ported to tests/conformance/conformance.py for
# unit-testability and to close Plan-A gap 5.1 (the missing PASS>0 silent-green
# guard: the bash loop read nothing ⇒ PASS=0/FAIL=0 ⇒ exit 0, a green over a
# run that validated nothing). This wrapper keeps the documented
# `run_conformance.sh <bezelc>` interface stable for callers/docs (CLAUDE.md,
# the gate wiring, the 0022 proposal) so the port needs no companion churn.
#
# Memory discipline: every bezelc call and every compiled-program run is caged
# (inside the Python) at MemoryMax=4G, no swap, under `timeout 30`, within one
# delegated scope in bezel-heavy.slice per run (conformance.py, "The per-case
# cage"). Cases are processed SEQUENTIALLY.
#
# Exit code: 0 iff every non-skip case passes AND at least one case passed
# (Plan-A 5.1); 1 otherwise.
exec python3 "$(dirname "${BASH_SOURCE[0]}")/run_conformance.py" "$@"
