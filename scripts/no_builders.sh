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
# Usage:  scripts/no_builders.sh        # table + verdict
#         scripts/no_builders.sh -q     # verdict only
# Exit 0 = nothing building. Exit 1 = builders present (do NOT gate).
set -uo pipefail

QUIET=0
[ "${1:-}" = "-q" ] && QUIET=1

# comm can be truncated, so match by PREFIX for the long ones (bezel_tests_*).
snapshot="$(ps -eo comm= 2>/dev/null)"
total=0
report=""
while IFS= read -r spec; do
  [ -z "$spec" ] && continue
  name="${spec%%:*}"; pat="${spec#*:}"
  n="$(printf '%s\n' "$snapshot" | grep -cE "$pat")"
  n="${n:-0}"
  total=$(( total + n ))
  [ "$n" -gt 0 ] && report="${report}$(printf '  %-18s %d' "$name" "$n")"$'\n'
done <<'SPECS'
clang:^clang
cc1:^cc1
make:^(make|gmake|ninja)$
cmake:^cmake$
linker:^ld\.(mold|lld)$
bezel_tests:^bezel_tests
bezelc:^bezelc$
SPECS

if [ "$QUIET" -eq 0 ] && [ -n "$report" ]; then printf '%s' "$report"; fi
if [ "$total" -eq 0 ]; then
  echo "no_builders: CLEAN — nothing building"
  exit 0
fi
echo "no_builders: $total builder process(es) running — do NOT launch a gate"
exit 1
