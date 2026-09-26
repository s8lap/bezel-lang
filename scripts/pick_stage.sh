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
#       --lane <lane-branch> --reviewer <id> --verdict <ACCEPT|ACCEPT-W-NOTES> SHA...
# Runs in the CURRENT worktree and refuses unless its branch is --branch.
#
# Exit: 0 all picked · 2 cherry-pick conflict (status printed, pick in progress)
#       3 trailer violation (commit left in place, no note written)
#       64 usage / refusal · 1 other git error
#
# Hooks (scripts/hooks/pre-commit, worktree ownership): measured on git 2.55,
# `git cherry-pick` does NOT run pre-commit, but the `--amend` below DOES. So in
# a worktree claimed with scripts/wt_owner.sh the amend is checked against the
# owner's recorded branch; that passes whenever --branch matches the claim.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TRAILERS="$HERE/check_commit_trailers.py"

usage() { sed -n '22,29p' "$0" >&2; exit 64; }
die() { echo "pick_stage: $*" >&2; exit 64; }

BRANCH="" POOL="" LANE="" REVIEWER="" VERDICT=""
SHAS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --branch) BRANCH="${2:-}"; shift 2 ;;
    --pool) POOL="${2:-}"; shift 2 ;;
    --lane) LANE="${2:-}"; shift 2 ;;
    --reviewer) REVIEWER="${2:-}"; shift 2 ;;
    --verdict) VERDICT="${2:-}"; shift 2 ;;
    -h|--help) usage ;;
    -*) die "unknown flag '$1'" ;;
    *) SHAS+=("$1"); shift ;;
  esac
done

for f in BRANCH POOL LANE REVIEWER VERDICT; do
  [ -n "${!f}" ] || die "--$(echo "$f" | tr 'A-Z' 'a-z') is required"
done
case "$VERDICT" in ACCEPT|ACCEPT-W-NOTES) ;; *) die "--verdict must be ACCEPT or ACCEPT-W-NOTES, got '$VERDICT'" ;; esac
[ "${#SHAS[@]}" -gt 0 ] || die "no SHA to pick"

cur="$(git branch --show-current)"
[ "$cur" = "$BRANCH" ] || die "current branch is '${cur:-(detached)}', expected '$BRANCH' — refusing (cd into the staging worktree first)"

# Resolve every SHA before touching anything, so a typo cannot leave a half batch.
ORIGS=()
for c in "${SHAS[@]}"; do
  full="$(git rev-parse --verify --quiet "$c^{commit}")" || die "cannot resolve '$c' to a commit"
  ORIGS+=("$full")
done

TMP="$(mktemp -d "${TMPDIR:-/var/tmp}/pick_stage.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

for orig in "${ORIGS[@]}"; do
  if ! git cherry-pick -x "$orig" >"$TMP/pick.log" 2>&1; then
    cat "$TMP/pick.log" >&2
    echo "pick_stage: CONFLICT picking ${orig:0:10} — resolve or 'git cherry-pick --abort'" >&2
    git status --short >&2
    exit 2
  fi
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
    echo "pick_stage: TRAILER VIOLATION on ${new:0:10} (from ${orig:0:10}) — commit left in place," >&2
    echo "  no note written. Amend it by hand or 'git reset --hard HEAD~1', then re-run." >&2
    exit 3
  fi

  issues="$(grep -oE '#[0-9]{3,}' "$TMP/msg" | awk '!seen[$0]++' | paste -sd' ' || true)"
  {
    echo "pool: $POOL"
    echo "lane: $LANE"
    echo "orig: $orig"
    echo "reviewer: $REVIEWER"
    echo "verdict: $VERDICT"
    echo "picked-at: $(date -Iseconds)"
    echo "issues: $issues"
  } > "$TMP/note"
  git notes --ref=bezel-picks add -f -F "$TMP/note" "$new" >/dev/null 2>&1 \
    || { echo "pick_stage: could not write note on ${new:0:10}" >&2; exit 1; }
done
echo "pick_stage: ${#ORIGS[@]} commit(s) picked onto $BRANCH, notes in refs/notes/bezel-picks"
