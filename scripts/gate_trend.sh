#!/usr/bin/env bash
# gate_trend.sh — read gate-timings.jsonl back and SHOUT when a gate's own
# telemetry contradicts its verdict (#3010).
#
# This script is now a thin wrapper over the Python module scripts/gate/trend.py
# (the five structural rules R1-R5 and the scan-line / exit-code contract were
# lifted verbatim from this file's former inline heredoc; see the module for the
# full WHY and the rule-by-rule rationale). The migration used the
# emit-alongside -> prove-equivalence -> switch methodology: the module was
# shown byte-identical to the old heredoc (zero stdout diff, matching exit
# codes) over all 488 historical records before this script switched to exec it.
#
# The CLI surface is preserved exactly: --log, --limit, --last, -h/--help;
# exit codes 0 (no rule fired) / 1 (FAIL finding or malformed newest record) /
# 2 (unreadable log) / 64 (unknown argument); and the unconditional
# "trend: N records scanned, M rules evaluated" scan line that callers
# (gate.sh advisory, gate_and_push.sh blocking) grep for as proof this ran.
HERE="$(cd "$(dirname "$0")" && pwd)"
PYTHONPATH="$HERE${PYTHONPATH:+:$PYTHONPATH}" exec python3 -m gate.trend "$@"
