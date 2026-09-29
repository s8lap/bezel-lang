#!/usr/bin/env bash
# wt_owner.sh — bind a worktree to its owner and branch.
#
# WHY. In R21 another session's lane agents ran `git switch <their-branch>`
# inside worktrees a different session owned, and were about to commit there.
# A worktree has no notion of who it belongs to, so nothing noticed. This
# records the owner and the branch the owner expects in the PER-WORKTREE git
# dir (`git rev-parse --git-dir`/bezel-owner — not the common dir, so each
# worktree has its own binding), and the hooks in scripts/hooks/ read it:
#   pre-commit     REFUSES a commit when the current branch != the recorded one
#   post-checkout  WARNS loudly on a switch to another branch (it cannot block)
# Hooks are installed once per repo by scripts/install_hooks.sh (coordinator's
# decision). With no bezel-owner file both hooks are no-ops.
#
# Usage:  scripts/wt_owner.sh claim <owner-name>   # records owner + current branch
#         scripts/wt_owner.sh show                 # prints the binding (exit 1 if none)
#         scripts/wt_owner.sh release              # removes it
# File format (key: value lines): owner, branch, claimed-at.
set -euo pipefail

GITDIR="$(git rev-parse --path-format=absolute --git-dir 2>/dev/null)" \
  || { echo "wt_owner: not inside a git worktree" >&2; exit 64; }
F="$GITDIR/bezel-owner"

case "${1:-show}" in
  claim)
    owner="${2:-}"
    [ -n "$owner" ] || { echo "usage: $0 claim <owner-name>" >&2; exit 64; }
    br="$(git branch --show-current)"
    [ -n "$br" ] || { echo "wt_owner: detached HEAD — check out the branch to bind first" >&2; exit 64; }
    printf 'owner: %s\nbranch: %s\nclaimed-at: %s\n' "$owner" "$br" "$(date -Iseconds)" > "$F"
    echo "wt_owner: $(git rev-parse --show-toplevel) claimed by '$owner' on branch '$br'"
    ;;
  show)
    [ -f "$F" ] || { echo "wt_owner: no owner recorded ($F)"; exit 1; }
    cat "$F"
    ;;
  release)
    rm -f "$F"; echo "wt_owner: released"
    ;;
  *) echo "usage: $0 {claim <owner>|show|release}" >&2; exit 64 ;;
esac
