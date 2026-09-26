#!/usr/bin/env bash
# pick_ledger.sh — the batch ledger, read back from the notes pick_stage.sh writes.
#
# WHY. CLAUDE.md asks the integrator to keep "a batch ledger naming the authoring
# pool" so a gate FAIL can be routed to the coordinator who holds its context.
# In R21 that ledger was hand-kept, and "how many fixes are banked" was a
# hand-tally across three pools. scripts/pick_stage.sh now records pool / lane /
# reviewer / verdict / issues as a note under refs/notes/bezel-picks on every
# picked commit; this prints them.
#
# Usage:  scripts/pick_ledger.sh [RANGE]        (default origin/main..HEAD)
#         scripts/pick_ledger.sh 466a28e2..origin/main   # a landed range works too
#
# Output: one row per commit, oldest first (short sha, pool, lane, verdict,
# issues, subject), then a summary:
#   * commits per pool;
#   * distinct issues per pool — each issue is credited to the pool of its
#     EARLIEST commit in the range, so a follow-up by another pool does not
#     double-count it;
#   * `distinct issues: N` (the "fixes banked" number run_status.sh reads);
#   * commits with no note, flagged UNATTRIBUTED. Their issues are still taken
#     from the commit message and credited to the pseudo-pool UNATTRIBUTED.
# An issue is `#NNN` or longer (three or more digits), as pick_stage.sh records it.
# Read-only. Works from any worktree (notes live in the common git dir).
set -uo pipefail

RANGE="${1:-origin/main..HEAD}"
REF=bezel-picks

shas="$(git rev-list --reverse "$RANGE" 2>/dev/null)" || {
  echo "pick_ledger: cannot list '$RANGE'" >&2; exit 1; }

declare -A COMMITS=() ISSUES=() CREDIT=()
unattr=0 total=0
printf '%-10s %-12s %-24s %-15s %-18s %s\n' SHA POOL LANE VERDICT ISSUES SUBJECT
for sha in $shas; do
  total=$((total + 1))
  note="$(git notes --ref="$REF" show "$sha" 2>/dev/null || true)"
  field() { printf '%s\n' "$note" | sed -n "s/^$1: *//p" | head -n 1; }
  if [ -n "$note" ]; then
    pool="$(field pool)"; lane="$(field lane)"; verdict="$(field verdict)"
    issues="$(field issues)"
    [ -n "$pool" ] || pool="?"
  else
    pool=UNATTRIBUTED; lane=-; verdict=-
    issues="$(git log -1 --format=%B "$sha" | grep -oE '#[0-9]{3,}' | awk '!s[$0]++' | paste -sd' ')"
    unattr=$((unattr + 1))
  fi
  COMMITS[$pool]=$(( ${COMMITS[$pool]:-0} + 1 ))
  for i in $issues; do
    [ -n "${CREDIT[$i]:-}" ] && continue   # earliest commit in the range wins
    CREDIT[$i]="$pool"
    ISSUES[$pool]=$(( ${ISSUES[$pool]:-0} + 1 ))
  done
  printf '%-10s %-12s %-24s %-15s %-18s %s\n' "${sha:0:10}" "${pool:0:12}" "${lane:0:24}" \
    "$verdict" "${issues:--}" "$(git log -1 --format=%s "$sha" | cut -c1-70)"
done

echo
echo "-- summary for $RANGE: $total commit(s)"
printf '  %-14s %8s %16s\n' POOL COMMITS "ISSUES(credited)"
for p in $(printf '%s\n' "${!COMMITS[@]}" | sort); do
  printf '  %-14s %8d %16d\n' "$p" "${COMMITS[$p]}" "${ISSUES[$p]:-0}"
done
echo "distinct issues: ${#CREDIT[@]}"
if [ "$unattr" -gt 0 ]; then
  echo "UNATTRIBUTED: $unattr commit(s) have no refs/notes/$REF note (picked without scripts/pick_stage.sh)"
else
  echo "UNATTRIBUTED: 0"
fi
