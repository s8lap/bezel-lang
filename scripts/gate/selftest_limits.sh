# scripts/gate/selftest_limits.sh — SOURCED, never executed. The one place the
# gate self-test skip cap lives, so gate_full_cycle.sh and pregate.sh cannot
# drift apart (R39 gate 1: the pregate said "OK (skipped=6)" PASS while the
# cycle's cap of 4 failed the same tests 8 minutes in).
#
# GATE_SELFTEST_SKIP_CAP — most tests `python3 -m unittest discover -s
#   scripts/gate/tests` may skip INSIDE A GATE before the cycle fails.
# GATE_SELFTEST_GATE_ONLY_SKIPS — skips that happen ONLY inside a gate (or on a
#   dirty tree): the two test_integration_bash_python tests that drive the real
#   gate_and_push.sh / lockfile self-skip when a gate is running or the tree
#   has tracked changes (_live_gate_precondition_unmet). pregate runs on a
#   clean tree with no gate live, so it must see that many FEWER skips.
GATE_SELFTEST_SKIP_CAP=4
GATE_SELFTEST_GATE_ONLY_SKIPS=2

# gate_selftest_skip_cap_for <inside-gate:1|0>  -> prints the cap to enforce.
gate_selftest_skip_cap_for() {
  if [ "${1:-1}" = 1 ]; then echo "$GATE_SELFTEST_SKIP_CAP"
  else echo $((GATE_SELFTEST_SKIP_CAP - GATE_SELFTEST_GATE_ONLY_SKIPS)); fi
}

# gate_selftest_skip_count <unittest-log> -> prints N from "OK (skipped=N)", else 0.
# The same sed gate_full_cycle.sh applies to its own log.
gate_selftest_skip_count() {
  local n
  n="$(sed -n 's/^OK (.*skipped=\([0-9]*\).*/\1/p' "$1" 2>/dev/null | tail -1)"
  echo "${n:-0}"
}
