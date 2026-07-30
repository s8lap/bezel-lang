#!/usr/bin/env bash
#
# check_issue_refs.sh — audit the GitHub issue references in a commit range.
#
# Why this exists. Two failure modes bit this project on 2026-07-30, both silent:
#
#   1. GitHub's commit-message auto-close is ERRATIC, not merely absent. Across
#      two pushes with identically-formed `fix #NNNN:` subjects, one closed
#      0-of-9 issues and the next closed 1-of-5. That is worse than consistent
#      failure, because a spot check passes and you conclude it works. Every
#      referenced issue has to be verified after a push, and closed by hand.
#
#   2. A DANGLING reference. A lane's second commit corrected an issue-number
#      typo in its first; cherry-picking replayed the typo'd original and
#      dropped the correction (a message-only commit cherry-picks as EMPTY), so
#      main shipped a reference to #2439, which does not exist. A dead pointer
#      in the history is permanent and nobody notices it.
#
# This script is READ-ONLY on purpose. It does not close anything: the close
# comment is where PARTIAL scope gets stated ("the crash claim was stale, only
# the OOM residual was real"; "the itable-dispatched shape is deferred to
# #2422"), and a generic auto-comment would overstate what shipped. Roughly a
# third of fixes in a typical batch are partial. So this prints the checklist
# and you write the comments.
#
# Usage:
#   scripts/check_issue_refs.sh                 # origin/main..HEAD (pre-push)
#   scripts/check_issue_refs.sh <range>         # any git range (post-push audit)
#
# Exit: 1 if any reference is dangling (a hard error worth blocking on),
#       0 otherwise — still-open issues are reported but are the normal case
#       pre-push, so they do not fail the run.

set -uo pipefail

RANGE="${1:-origin/main..HEAD}"
command -v gh >/dev/null || { echo "check_issue_refs: gh not found" >&2; exit 2; }
git rev-parse "$RANGE" >/dev/null 2>&1 || { echo "check_issue_refs: bad range '$RANGE'" >&2; exit 2; }

# Collect refs as "<issue> <short-sha> <subject>", one line per (issue, commit).
# Scan the FULL message (subject + body): a "Filed #NNNN" line in the body is
# exactly where the dangling-reference bug lived.
#
# Done with a per-commit loop rather than one clever awk pass: a commit body is
# multi-line and may contain tabs, so record/field splitting over a single
# `git log` stream misaligns the columns (it produced rows keyed by SHA instead
# of issue number on the first attempt). One `git log -1` per commit is a few
# milliseconds and is obviously correct.
rows="$(
  for sha in $(git rev-list --abbrev-commit "$RANGE"); do
    subj="$(git log -1 --format=%s "$sha")"
    # An issue named in the SUBJECT is one this commit claims to fix -> must be
    # closed after the push. One named only in the BODY is usually a follow-up
    # the lane filed, or a related fix -> must stay OPEN. Conflating the two is
    # how a just-filed issue gets closed by mistake.
    git log -1 --format=%B "$sha" | grep -oE '#[0-9]{4,}' | tr -d '#' | sort -u |
      while read -r n; do
        case "$subj" in *"#$n"*) kind=FIX ;; *) kind=mention ;; esac
        printf '%s\t%s\t%s\t%s\n' "$n" "$sha" "$kind" "$subj"
      done
  done | sort -u -k1,1n
)"

[ -n "$rows" ] || { echo "check_issue_refs: no issue references in $RANGE"; exit 0; }

dangling=0; open_n=0; closed_n=0; to_close=""
printf '%-8s %-8s %-8s %-10s %s\n' "ISSUE" "STATE" "ROLE" "COMMIT" "SUBJECT"
printf '%-8s %-8s %-8s %-10s %s\n' "-----" "-----" "----" "------" "-------"
while IFS=$'\t' read -r num sha kind subj; do
  [ -n "$num" ] || continue
  if ! state="$(gh issue view "$num" --json state -q .state 2>/dev/null)"; then
    state="DANGLING"; dangling=$(( dangling + 1 ))
  elif [ "$state" = OPEN ]; then open_n=$(( open_n + 1 ))
  else closed_n=$(( closed_n + 1 ))
  fi
  [ "$kind" = FIX ] && [ "$state" = OPEN ] && to_close="$to_close $num"
  printf '#%-7s %-8s %-8s %-10s %.52s\n' "$num" "$state" "$kind" "$sha" "$subj"
done <<< "$rows"

echo
echo "check_issue_refs: $closed_n closed, $open_n open, $dangling dangling  (range: $RANGE)"
if [ "$dangling" -gt 0 ]; then
  echo "check_issue_refs: ERROR — $dangling reference(s) point at an issue that does not exist." >&2
  echo "  A dead pointer in history is permanent. If the commit is not yet pushed, fix it with" >&2
  echo "  \`git commit --amend\` / \`git rebase -i\`. NB a separate message-only 'fixup' commit does" >&2
  echo "  NOT work — it cherry-picks as empty and its correction is silently lost." >&2
  exit 1
fi
if [ -n "$to_close" ]; then
  echo "check_issue_refs: CLOSE AFTER PUSH (subject claims a fix, still open):$to_close"
  echo "  State any PARTIAL scope in the comment — roughly a third of fixes ship partial."
fi
echo "check_issue_refs: OPEN issues with role 'mention' are follow-ups a lane filed — leave them open."
exit 0
