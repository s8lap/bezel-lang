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
# kind, issues, subject), then a summary:
#   * commits per pool;
#   * fix issues per pool — issues of kind=fix commits only, each credited to
#     the pool of its EARLIEST fix commit in the range, so a follow-up by
#     another pool does not double-count it;
#   * `distinct fix issues: N` (the "fixes banked" number run_status.sh reads),
#     and how many of them rest on an INFERRED kind;
#   * `distinct issues named (all kinds)`, for reference;
#   * commits with no note, flagged UNATTRIBUTED. Their issues are still taken
#     from the commit SUBJECT and credited to the pseudo-pool UNATTRIBUTED.
# NOTE RECOVERY. Notes are keyed by SHA, and a rebase of staging rewrites every
# SHA; git carries notes across only with
#     git -c notes.rewriteRef=refs/notes/bezel-picks rebase …
# (per command — never `git config`). When a commit has no note by SHA, the
# ledger looks for a noted commit with the same `git patch-id --stable` (the
# pre-rebase pick, still annotated) and uses that note; such a row's KIND is
# shown with a trailing `~` and counts as recorded. Only a commit with no note
# by SHA OR patch-id is UNATTRIBUTED.
# Which issues a commit counts toward is decided in ONE place,
# scripts/pick_issues.sh: the #NNN in the SUBJECT only (bodies are full of
# cross-references). A subject without one counts 0. The KIND column is the
# note's recorded kind; with no recorded kind it is inferred from the subject
# (pick_issues.sh --infer-kind) and shown with a trailing `*`.
# Read-only. Works from any worktree (notes live in the common git dir).
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RANGE="${1:-origin/main..HEAD}"
REF=bezel-picks

shas="$(git rev-list --reverse "$RANGE" 2>/dev/null)" || {
  echo "pick_ledger: cannot list '$RANGE'" >&2; exit 1; }

declare -A COMMITS=() FIXES=() CREDIT=() ALL=() PID_NOTE=() PID_OF=()
unattr=0 total=0 inferred_fix_issues=0 recovered=0

patch_ids() { # SHAs on stdin -> "patchid sha" lines (merges and empty commits give none)
  git log --no-walk=unsorted --stdin -p --format='commit %H' 2>/dev/null | git patch-id --stable 2>/dev/null
}
MAP_BUILT=0
build_patch_map() { # patch-id -> annotated commit, over EVERY note; then the range's own patch-ids
  MAP_BUILT=1
  local pid c
  while read -r pid c; do
    [ -n "${PID_NOTE[$pid]:-}" ] || PID_NOTE[$pid]="$c"
  done < <(git notes --ref="$REF" list 2>/dev/null | awk '{print $2}' | patch_ids)
  while read -r pid c; do PID_OF[$c]="$pid"; done < <(printf '%s
' $shas | patch_ids)
}
printf '%-10s %-12s %-22s %-14s %-7s %-18s %s\n' SHA POOL LANE VERDICT KIND ISSUES SUBJECT
for sha in $shas; do
  total=$((total + 1))
  note="$(git notes --ref="$REF" show "$sha" 2>/dev/null || true)"
  field() { printf '%s\n' "$note" | sed -n "s/^$1: *//p" | head -n 1; }
  kind="" mark=""
  if [ -z "$note" ]; then
    [ "$MAP_BUILT" = 1 ] || build_patch_map
    pid="${PID_OF[$sha]:-}"
    src="${pid:+${PID_NOTE[$pid]:-}}"
    if [ -n "$src" ] && [ "$src" != "$sha" ]; then
      note="$(git notes --ref="$REF" show "$src" 2>/dev/null || true)"
      [ -n "$note" ] && { mark="~"; recovered=$((recovered + 1)); }
    fi
  fi
  if [ -n "$note" ]; then
    pool="$(field pool)"; lane="$(field lane)"; verdict="$(field verdict)"
    issues="$(field issues)"; kind="$(field kind)"
    [ -n "$pool" ] || pool="?"
  else
    pool=UNATTRIBUTED; lane=-; verdict=-
    issues="$("$HERE/pick_issues.sh" "$sha")"
    unattr=$((unattr + 1))
  fi
  # Recorded kind wins; otherwise infer it, and mark the row with `*`.
  shown="$kind${kind:+$mark}"
  if [ -z "$kind" ]; then kind="$("$HERE/pick_issues.sh" --infer-kind "$sha")"; shown="$kind*"; fi
  COMMITS[$pool]=$(( ${COMMITS[$pool]:-0} + 1 ))
  for i in $issues; do ALL[$i]=1; done
  if [ "$kind" = fix ]; then
    for i in $issues; do
      [ -n "${CREDIT[$i]:-}" ] && continue   # earliest FIX commit in the range wins
      CREDIT[$i]="$pool"
      FIXES[$pool]=$(( ${FIXES[$pool]:-0} + 1 ))
      [ "$shown" = "fix*" ] && inferred_fix_issues=$((inferred_fix_issues + 1))
    done
  fi
  printf '%-10s %-12s %-22s %-14s %-7s %-18s %s\n' "${sha:0:10}" "${pool:0:12}" "${lane:0:22}" \
    "$verdict" "$shown" "${issues:--}" "$(git log -1 --format=%s "$sha" | cut -c1-70)"
done

echo
echo "-- summary for $RANGE: $total commit(s)   (KIND with * = inferred from the subject, no recorded kind;"
echo "   KIND with ~ = note recovered by patch-id after a rebase: $recovered row(s))"
printf '  %-14s %8s %20s\n' POOL COMMITS "FIX-ISSUES(credited)"
for p in $(printf '%s\n' "${!COMMITS[@]}" | sort); do
  printf '  %-14s %8d %20d\n' "$p" "${COMMITS[$p]}" "${FIXES[$p]:-0}"
done
echo "distinct fix issues: ${#CREDIT[@]} ($inferred_fix_issues from inferred kinds)"
echo "distinct issues named (all kinds): ${#ALL[@]}"
if [ "$unattr" -gt 0 ]; then
  echo "UNATTRIBUTED: $unattr commit(s): no refs/notes/$REF note found by SHA or patch-id"
else
  echo "UNATTRIBUTED: 0"
fi
