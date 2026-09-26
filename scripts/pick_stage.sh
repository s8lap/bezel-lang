#!/usr/bin/env bash
# pick_stage.sh — cherry-pick reviewed lane commits onto the staging branch and
# record WHO authored and reviewed each one as a git note.
#
# WHY. In R21 three coordinator sessions fed one staging branch. The batch
# ledger that CLAUDE.md asks for ("keep a batch ledger naming the authoring
# pool") lived in scratch files and chat, so on a gate FAIL the integrator had
# to re-derive which pool, lane and reviewer each commit came from, and the
# "fixes banked" count was hand-tallied. The commit trailer cannot carry this
# (exactly one mandated trailer), so it goes in a note on the PICKED commit:
#   refs/notes/bezel-picks   (lives in the common git dir: every worktree sees it)
# `scripts/pick_ledger.sh` reads it back.
#
# This replaces an ad-hoc scratch pick script and keeps its behaviour: pick with
# -x, drop the "(cherry picked from …)" line again (the note records `orig`),
# re-author as the integrator (--reset-author), and print one line per pick.
# New: after each pick the commit MUST end with exactly the one mandated
# trailer, checked by scripts/check_commit_trailers.py (the single source of
# the WANT string). A violation stops the run with exit 3 and leaves the commit
# in place for a human to amend — it is never silently rewritten.
#
# Usage:
#   scripts/pick_stage.sh --branch <expected-staging-branch> --pool <name> \
#       --lane <lane-branch> --reviewer <id> --verdict <ACCEPT|ACCEPT-W-NOTES> \
#       --kind <fix|test|bank|infra> [--continue] SHA...
# --kind is RECORDED, not inferred: only kind=fix commits count toward "fixes
# banked" (pick_ledger.sh, run_status.sh), since banking/test/infra subjects
# routinely name an issue too. All SHAs in one call share one kind.
# Runs in the CURRENT worktree and refuses unless its branch is --branch.
# Refuses to start on a tree with uncommitted tracked changes. A pick that
# turns out EMPTY (already applied) is skipped (`git cherry-pick --skip`).
#
# CONFLICT (exit 2): the pick is left in progress and the exact recipe is
# printed, with the SHAs not yet picked. Resolve, `git add` the paths, then
#   scripts/pick_stage.sh --continue <the same flags> [remaining SHA...]
# which runs `git cherry-pick --continue` (unless you already did), then does
# what a clean pick does — drop the -x line, re-author, check the trailer,
# write the note on HEAD — and picks the remaining SHAs. Skipping this step
# leaves the "(cherry picked from …)" line (the gate's trailer phase fails)
# and no note.
#
# Exit: 0 all picked · 2 conflict (pick in progress, recipe printed)
#       3 trailer violation (commit left in place, no note written)
#       64 usage / refusal · 1 other git error
#
# REBASING staging drops notes unless you pass, per command (never git config):
#   git -c notes.rewriteRef=refs/notes/bezel-picks rebase …
# pick_ledger.sh recovers a lost note by patch-id and marks the row `~`.
#
# Hooks (scripts/hooks/pre-commit, worktree ownership): measured on git 2.55,
# `git cherry-pick` does NOT run pre-commit, but the `--amend` below DOES. So in
# a worktree claimed with scripts/wt_owner.sh the amend is checked against the
# owner's recorded branch; that passes whenever --branch matches the claim.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TRAILERS="$HERE/check_commit_trailers.py"

usage() { sed -n '22,43p' "$0" >&2; exit 64; }
die() { echo "pick_stage: $*" >&2; exit 64; }

BRANCH="" POOL="" LANE="" REVIEWER="" VERDICT="" KIND="" CONTINUE=0
SHAS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --branch|--pool|--lane|--reviewer|--verdict|--kind)
      if [ $# -lt 2 ] || [ -z "$2" ] || [ "${2#--}" != "$2" ]; then
        die "$1 needs a value"
      fi
      case "$1" in
        --branch) BRANCH="$2" ;;
        --pool) POOL="$2" ;;
        --lane) LANE="$2" ;;
        --reviewer) REVIEWER="$2" ;;
        --verdict) VERDICT="$2" ;;
        --kind) KIND="$2" ;;
      esac
      shift 2 ;;
    --continue) CONTINUE=1; shift ;;
    -h|--help) usage ;;
    -*) die "unknown flag '$1'" ;;
    *) SHAS+=("$1"); shift ;;
  esac
done

for f in BRANCH POOL LANE REVIEWER VERDICT KIND; do
  [ -n "${!f}" ] || die "--$(echo "$f" | tr 'A-Z' 'a-z') is required"
done
case "$VERDICT" in ACCEPT|ACCEPT-W-NOTES) ;; *) die "--verdict must be ACCEPT or ACCEPT-W-NOTES, got '$VERDICT'" ;; esac
case "$KIND" in fix|test|bank|infra) ;; *) die "--kind must be fix, test, bank or infra, got '$KIND'" ;; esac
[ "$CONTINUE" -eq 1 ] || [ "${#SHAS[@]}" -gt 0 ] || die "no SHA to pick"

cur="$(git branch --show-current)"
[ "$cur" = "$BRANCH" ] || die "current branch is '${cur:-(detached)}', expected '$BRANCH' — refusing (cd into the staging worktree first)"

in_pick() { git rev-parse -q --verify CHERRY_PICK_HEAD >/dev/null; }
if in_pick; then
  [ "$CONTINUE" -eq 1 ] || die "a cherry-pick is in progress — finish it with --continue <same flags>, or 'git cherry-pick --abort'"
elif [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  die "the tree has uncommitted tracked changes — refusing to pick onto a dirty tree"
fi

# Resolve every SHA before touching anything, so a typo cannot leave a half batch.
ORIGS=()
for c in "${SHAS[@]}"; do
  full="$(git rev-parse --verify --quiet "$c^{commit}")" || die "cannot resolve '$c' to a commit"
  ORIGS+=("$full")
done

TMP="$(mktemp -d "${TMPDIR:-/var/tmp}/pick_stage.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# Flags to repeat on --continue (every value is validated above: no spaces).
FLAGS="--branch $BRANCH --pool $POOL --lane $LANE --reviewer $REVIEWER --verdict $VERDICT --kind $KIND"

finalize() { # $1 = orig. HEAD is orig's fresh cherry-pick: normalise, check, note.
  local orig="$1" new n_tr last issues
  # Drop the -x line (the note carries `orig`), rstrip, keep one final newline.
  git log -1 --format=%B HEAD \
    | grep -vE '^\(cherry picked from commit [0-9a-f]+\)$' \
    | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' > "$TMP/msg"
  git commit -q --amend --reset-author -F "$TMP/msg"
  new="$(git rev-parse HEAD)"

  n_tr="$(grep -c '^Co-Authored-By:' "$TMP/msg" || true)"
  last="$(tail -n 1 "$TMP/msg")"
  echo "${orig:0:10} -> ${new:0:10} trailers=$n_tr last=$last author=$(git log -1 --format=%an HEAD) :: $(head -n 1 "$TMP/msg")"

  if ! python3 "$TRAILERS" --range "$new^..$new" > "$TMP/tr.log" 2>&1; then
    cat "$TMP/tr.log" >&2
    echo "pick_stage: TRAILER VIOLATION on ${new:0:10} (from ${orig:0:10}) — commit left in place, no note written." >&2
    echo "  The lane's commit is the source: fix its trailer ON THE LANE BRANCH, then" >&2
    echo "  'git reset --hard HEAD~1' here on $BRANCH and re-pick the lane's new SHA." >&2
    exit 3
  fi

  issues="$("$HERE/pick_issues.sh" < "$TMP/msg")"   # subject-only rule, one place
  {
    echo "pool: $POOL"
    echo "lane: $LANE"
    echo "orig: $orig"
    echo "reviewer: $REVIEWER"
    echo "verdict: $VERDICT"
    echo "kind: $KIND"
    echo "picked-at: $(date -Iseconds)"
    echo "issues: $issues"
  } > "$TMP/note"
  git notes --ref=bezel-picks add -f -F "$TMP/note" "$new" >/dev/null 2>&1 \
    || { echo "pick_stage: could not write note on ${new:0:10}" >&2; exit 1; }
}

picked=0 skipped=0
if [ "$CONTINUE" -eq 1 ]; then
  if in_pick; then
    orig="$(git rev-parse CHERRY_PICK_HEAD)"
    if git diff --quiet && git diff --cached --quiet; then
      # The resolution kept nothing of the pick: it is already applied.
      git cherry-pick --skip >/dev/null 2>&1 || true
      echo "${orig:0:10} already applied, skipped (empty after resolution)"
      skipped=$((skipped + 1))
    elif ! git -c core.editor=true cherry-pick --continue >"$TMP/pick.log" 2>&1; then
      cat "$TMP/pick.log" >&2
      git status --short >&2
      echo "pick_stage: 'git cherry-pick --continue' failed — resolve and 'git add' every conflicted path, then re-run --continue" >&2
      exit 2
    else
      finalize "$orig"; picked=$((picked + 1))
    fi
  else
    # The human already ran `git cherry-pick --continue`; -x recorded the origin.
    orig="$(git log -1 --format=%B HEAD | sed -n 's/^(cherry picked from commit \([0-9a-f]*\))$/\1/p' | tail -n 1)"
    [ -n "$orig" ] || die "no cherry-pick in progress and HEAD has no '(cherry picked from …)' line — nothing to continue"
    finalize "$orig"; picked=$((picked + 1))
  fi
fi

for idx in "${!ORIGS[@]}"; do
  orig="${ORIGS[$idx]}"
  if ! git cherry-pick -x "$orig" >"$TMP/pick.log" 2>&1; then
    if in_pick && git diff --quiet && git diff --cached --quiet; then
      git cherry-pick --skip >/dev/null 2>&1 || true
      echo "${orig:0:10} already applied, skipped (empty pick)"
      skipped=$((skipped + 1))
      continue
    fi
    cat "$TMP/pick.log" >&2
    if ! in_pick; then
      echo "pick_stage: cherry-pick of ${orig:0:10} failed with no pick in progress (an untracked file in the way?)" >&2
      exit 1
    fi
    rest="${ORIGS[*]:$((idx + 1))}"
    git status --short >&2
    cat >&2 <<EOF
pick_stage: CONFLICT picking ${orig:0:10}; the pick is in progress ($picked picked before it).
  Not yet picked: ${rest:-(none)}
  To finish: resolve the conflicts, 'git add' the paths, then run
    $0 --continue $FLAGS${rest:+ $rest}
  It completes the cherry-pick, strips the -x line, re-authors, checks the
  trailer, writes the note, then picks the rest. To give up: git cherry-pick --abort
EOF
    exit 2
  fi
  finalize "$orig"; picked=$((picked + 1))
done
echo "pick_stage: $picked commit(s) picked onto $BRANCH ($skipped already applied, skipped), notes in refs/notes/bezel-picks"
