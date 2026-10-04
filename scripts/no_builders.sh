#!/usr/bin/env bash
# no_builders.sh — is any build/test process running? Gate precondition.
#
# WHY THIS EXISTS. The documented check used to be
#     pgrep -x 'clang++|clang|cc1plus|bezel_tests|cmake|ld.mold|ld.lld'
# and it is unreliable in the one direction that matters. `pgrep -x` matches
# the process name as an ANCHORED EXTENDED REGEX, so `+` is a metacharacter:
# `pgrep -xc 'clang++'` returns 0 while 31 clang++ processes are running
# (measured 2026-09-21). The alternation above only ever fired via `cmake`,
# so a box compiling hard with no cmake alive reported CLEAN.
#
# That matters because one concurrent builder makes a gate untrustworthy —
# contention is indistinguishable from a real regression, and has produced a
# "shard crash / undercount" banner plus 53 phantom failures that all passed
# in isolation. A precondition that cannot fail is worse than none.
#
# Match on `ps -eo comm=` with fixed strings instead: a name is a name.
#
# A RUNNING GATE'S OWN PROCESSES ARE NOT INTRUDERS. run_status.sh (and every
# heartbeat) calls this while a gate runs, to catch a lane building DURING the
# gate. Counting the gate's own suite binaries and ASan links there flagged
# every live gate as "running WITH builders" (R26, 2026-10-04). The gate is
# launched `setsid nohup scripts/gate_and_push.sh`, so its pid is its session
# id, and every process it starts keeps that session (dev_heavy.sh and the
# conformance harness use `systemd-run --scope`, which runs the command in the
# caller's session; measured: all 9-16 builders of the a466b546 gate carried
# it). So: when the gate pidfile names a live process, builders in that
# session are reported as gate-owned and excluded from the verdict. A gate
# launched WITHOUT setsid owns no session of its own, nothing is excluded, and
# the old conservative verdict stands. pregate.sh refuses to run while a gate
# is live, so the exclusion never lets a second gate launch.
#
# Usage:  scripts/no_builders.sh        # table + verdict
#         scripts/no_builders.sh -q     # verdict only
# Env:    BEZEL_GATE_PIDFILE (default /var/tmp/bezel-scratch/gate.pid), as
#         gate_and_push.sh.
# Exit 0 = nothing building (gate-owned excluded). Exit 1 = builders present.
set -uo pipefail

QUIET=0
[ "${1:-}" = "-q" ] && QUIET=1

# The live gate's session id, or empty (see the header).
gate_sid=""
pidfile="${BEZEL_GATE_PIDFILE:-/var/tmp/bezel-scratch/gate.pid}"
if [ -f "$pidfile" ]; then
  gp="$(tr -dc '0-9' < "$pidfile" 2>/dev/null)"
  if [ -n "$gp" ] && kill -0 "$gp" 2>/dev/null; then gate_sid="$gp"; fi
fi

# comm can be truncated, so match by PREFIX for the long ones (bezel_tests_*).
# Each line is "SID COMM"; split it into the gate's session and everyone else.
table="$(ps -eo sid=,comm= 2>/dev/null)"
snapshot="$(printf '%s\n' "$table" | awk -v g="$gate_sid" 'g == "" || $1 != g { $1 = ""; sub(/^ /, ""); print }')"
gate_snapshot="$(printf '%s\n' "$table" | awk -v g="$gate_sid" 'g != "" && $1 == g { $1 = ""; sub(/^ /, ""); print }')"
total=0
gate_total=0
report=""
while IFS= read -r spec; do
  [ -z "$spec" ] && continue
  name="${spec%%:*}"; pat="${spec#*:}"
  n="$(printf '%s\n' "$snapshot" | grep -cE "$pat")"
  n="${n:-0}"
  g="$(printf '%s\n' "$gate_snapshot" | grep -cE "$pat")"
  g="${g:-0}"
  total=$(( total + n ))
  gate_total=$(( gate_total + g ))
  [ "$n" -gt 0 ] && report="${report}$(printf '  %-18s %d' "$name" "$n")"$'\n'
done <<'SPECS'
clang:^clang
cc1:^cc1
make:^(make|gmake|ninja|ninja-build)$
cmake:^cmake$
linker:^ld\.(mold|lld)$
bezel_tests:^bezel_tests
bezelc:^bezelc$
SPECS

if [ "$QUIET" -eq 0 ] && [ -n "$report" ]; then printf '%s' "$report"; fi
gate_note=""
[ "$gate_total" -gt 0 ] && gate_note=" ($gate_total gate-owned process(es) of gate pid $gate_sid excluded)"
if [ "$total" -eq 0 ]; then
  echo "no_builders: CLEAN — nothing building$gate_note"
  exit 0
fi
echo "no_builders: $total builder process(es) running — do NOT launch a gate$gate_note"
exit 1
