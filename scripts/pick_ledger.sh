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
# (per command — never `git config`); that is the primary path. As a
# BEST-EFFORT fallback, when a commit has no note by SHA the ledger looks for
# the pre-rebase pick: a noted commit with the same `git patch-id --stable`
# AND the same subject. Exactly one such commit -> its note is used, the KIND
# is shown with a trailing `~` and counts as recorded. Otherwise the row is
# UNATTRIBUTED; if noted commits share its patch-id but none or several have
# its subject (identical diffs, e.g. a fix, its revert, and a re-add under
# another subject), LANE reads `(ambiguous)` — it never borrows another
# commit's note. Recovery also fails when the rebase changed the patch
# (a conflict resolution) or the pre-rebase commit was pruned by gc; pruned
# note targets are skipped, not fatal.
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

declare -A COMMITS=() FIXES=() CREDIT=() ALL=() PID_CANDS=() PID_OF=() SUBJ=()
unattr=0 ambiguous=0 total=0 inferred_fix_issues=0 recovered=0

patch_ids() { # SHAs on stdin -> "patchid sha" lines (merges and empty commits give none)
  git log --no-walk=unsorted --stdin -p --format='commit %H' 2>/dev/null | git patch-id --stable 2>/dev/null
}
MAP_BUILT=0
build_patch_map() {
  # patch-id -> LIST of noted commits. Note targets whose object is gone (a
  # pruned pre-rebase commit) are dropped first: `git log --stdin` dies on the
  # first missing object, which would empty the whole map.
  MAP_BUILT=1
  local pid c targets
  targets="$(git notes --ref="$REF" list 2>/dev/null | awk '{print $2}' \
    | git cat-file --batch-check='%(objectname) %(objecttype)' 2>/dev/null \
    | awk '$2 == "commit" {print $1}')"
  [ -n "$targets" ] || return 0
  while IFS=$'\t' read -r c pid; do SUBJ[$c]="$pid"; done \
    < <(printf '%s\n' "$targets" | git log --no-walk=unsorted --stdin --format='%H%x09%s' 2>/dev/null)
  while read -r pid c; do PID_CANDS[$pid]+="$c "; done < <(printf '%s\n' "$targets" | patch_ids)
  while read -r pid c; do PID_OF[$c]="$pid"; done < <(printf '%s\n' $shas | patch_ids)
}
recover_note() { # $1 = sha with no note by SHA. Prints the source commit, or
  # returns 1 (nothing shares its patch-id) / 2 (candidates, but not exactly
  # one with the same SUBJECT: ambiguous — never guess, a duplicate diff under
  # another subject is a different pick).
  local sha="$1" pid c subj n=0 hit="" any=0
  pid="${PID_OF[$sha]:-}"
  [ -n "$pid" ] || return 1
  subj="$(git log -1 --format=%s "$sha")"
  for c in ${PID_CANDS[$pid]:-}; do
    [ "$c" = "$sha" ] && continue
    any=1
    [ "${SUBJ[$c]:-}" = "$subj" ] && { n=$((n + 1)); hit="$c"; }
  done
  [ "$any" = 1 ] || return 1
  [ "$n" = 1 ] || return 2
  printf '%s\n' "$hit"
}
printf '%-10s %-12s %-22s %-14s %-7s %-18s %s\n' SHA POOL LANE VERDICT KIND ISSUES SUBJECT
for sha in $shas; do
  total=$((total + 1))
  note="$(git notes --ref="$REF" show "$sha" 2>/dev/null || true)"
  field() { printf '%s\n' "$note" | sed -n "s/^$1: *//p" | head -n 1; }
  kind="" mark="" amb=0
  if [ -z "$note" ]; then
    [ "$MAP_BUILT" = 1 ] || build_patch_map
    src="$(recover_note "$sha")"; rc=$?
    if [ "$rc" = 0 ]; then
      note="$(git notes --ref="$REF" show "$src" 2>/dev/null || true)"
      [ -n "$note" ] && { mark="~"; recovered=$((recovered + 1)); }
    elif [ "$rc" = 2 ]; then
      amb=1
    fi
  fi
  if [ -n "$note" ]; then
    pool="$(field pool)"; lane="$(field lane)"; verdict="$(field verdict)"
    issues="$(field issues)"; kind="$(field kind)"
    [ -n "$pool" ] || pool="?"
  else
    pool=UNATTRIBUTED; lane=-; verdict=-
    [ "$amb" = 1 ] && { lane="(ambiguous)"; ambiguous=$((ambiguous + 1)); }
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
  echo "UNATTRIBUTED: $unattr commit(s): no refs/notes/$REF note found by SHA or patch-id" \
       "($ambiguous ambiguous: a patch-id shared by noted commits, none or several with the same subject)"
else
  echo "UNATTRIBUTED: 0"
fi
