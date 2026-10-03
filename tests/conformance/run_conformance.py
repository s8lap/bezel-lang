#!/usr/bin/env python3
"""CLI entry point for the conformance harness.

The harness logic lives in conformance.py (unit-tested, injectable run seam);
this is the thin command-line wrapper that run_conformance.sh execs and that
the gate (scripts/gate_full_cycle.sh) calls. Exit code: 0 iff every non-skip
case passes AND at least one case passed (the Plan-A 5.1 silent-green guard);
1 otherwise.

Usage:  tests/conformance/run_conformance.py <bezelc-binary>
"""

import os
import sys

# Make the sibling conformance module importable when this file is run directly
# (e.g. `python3 tests/conformance/run_conformance.py …` or via the .sh wrapper).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import conformance  # noqa: E402

if __name__ == "__main__":
    conformance.enter_run_cage(__file__, sys.argv[1:])
    sys.exit(conformance.main(sys.argv[1:]))
