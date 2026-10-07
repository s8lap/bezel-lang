#!/usr/bin/env bash
# handback_check.sh — the mechanical half of accepting a lane's handback.
#
# WHY. In R21 every handback was verified by hand: is the reported SHA the
# branch tip, is the branch on the current staging base, one trailer per
# commit, no `git config` author leak, no files outside the lane's grant, no
# hardcoded checkout path in a new script, no stale needle for a renamed
# diagnostic, do the tests the commit message names exist. Each was skipped at
# least once (a reported SHA that was a dangling amend; a lane that committed as
# its own `git config` identity; a rename whose needles went vacuous). None of
# them needs judgement, so none of them should need the coordinator.
#
# Usage:
#   scripts/handback_check.sh <lane-branch> [--base REF]
#       [--reported-sha SHA] [--grant GLOB[,GLOB...]] [--author 'Name <email>']
#       [--repo PATH (default: this script's checkout)]
#
# --base defaults to $BEZEL_STAGING_REF when set, else origin/main. Mid-batch,
# pass the STAGING branch (`--base <staging-branch>`, or export
# BEZEL_STAGING_REF once): "behind origin/main" is not what an integrator
# picking onto staging needs to know.
#
# One PASS / WARN / FAIL / SKIP row per check, then `VERDICT:`.
# Exit 0 when nothing FAILed (WARNs are for the reviewer to read), 1 on a FAIL,
# 64 on bad usage. READ-ONLY: git -C and refs only, never a checkout, so it can
# be run from any worktree against any branch. It does look (read-only) for
# untracked scripts/ files in the worktree that has the branch checked out.
#
# --grant globs are bash patterns matched against repo-relative paths, where
# `*` also matches `/` (so `src/sema/*` covers subdirectories).
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(git -C "$HERE/.." rev-parse --show-toplevel 2>/dev/null)" || REPO=""
BRANCH="" BASE="${BEZEL_STAGING_REF:-origin/main}" REPORTED="" GRANT="" AUTHOR=""
while [ $# -gt 0 ]; do
  case "$1" in
    --base|--reported-sha|--grant|--author|--repo)
      if [ $# -lt 2 ] || [ -z "$2" ]; then
        echo "handback_check: $1 needs a value" >&2; exit 64
      fi
      case "$1" in
        --base) BASE="$2" ;; --reported-sha) REPORTED="$2" ;; --grant) GRANT="$2" ;;
        --author) AUTHOR="$2" ;; --repo) REPO="$2" ;;
      esac
      shift 2 ;;
    -h|--help) sed -n '14,30p' "$0"; exit 0 ;;
    -*) echo "handback_check: unknown flag '$1'" >&2; exit 64 ;;
    *) [ -z "$BRANCH" ] || { echo "handback_check: one branch only" >&2; exit 64; }
       BRANCH="$1"; shift ;;
  esac
done
[ -n "$BRANCH" ] || { echo "usage: $0 <lane-branch> [--base REF] [--reported-sha SHA] [--grant G,G] [--author 'N <e>']" >&2; exit 64; }
[ -n "$REPO" ] || { echo "handback_check: cannot derive a repo (#3413); pass --repo" >&2; exit 64; }

g() { git -C "$REPO" "$@"; }
NFAIL=0 NWARN=0
row() { # status check detail...
  local st="$1" name="$2"; shift 2
  [ "$st" = FAIL ] && NFAIL=$((NFAIL + 1))
  [ "$st" = WARN ] && NWARN=$((NWARN + 1))
  printf '%-4s  %-14s %s\n' "$st" "$name" "$*"
}
indent() { sed 's/^/                    /'; }

TIP="" BASESHA="" MB=""
FILES=""   # changed paths, base...branch

check_branch() {
  TIP="$(g rev-parse --verify --quiet "$BRANCH^{commit}")"
  if [ -z "$TIP" ]; then row FAIL branch "'$BRANCH' does not resolve to a commit"; return 1; fi
  row PASS branch "$BRANCH @ ${TIP:0:10}"
  BASESHA="$(g rev-parse --verify --quiet "$BASE^{commit}")"
  if [ -z "$BASESHA" ]; then row FAIL base "'$BASE' does not resolve"; return 1; fi
  # `git merge-base` exits non-zero when the histories are UNRELATED. That
  # non-zero was this function's return value, so `if check_branch; then`
  # skipped EVERY remaining check and the script fell through to
  # "VERDICT: PASS" -- the single case that must never pass reported green.
  # (a24-4976 carries root f667b8bf; r23-c28's root is e2aa55d0.)
  MB="$(g merge-base "$BASESHA" "$TIP")" || true
  if [ -z "$MB" ]; then
    row FAIL base "no common ancestor with $BASE -- UNRELATED HISTORIES (roots: $(g rev-list --max-parents=0 "$TIP" | head -1 | cut -c1-10) vs $(g rev-list --max-parents=0 "$BASESHA" | head -1 | cut -c1-10)); not pickable as-is"
    return 1
  fi
  return 0
}

check_reported() {
  [ -n "$REPORTED" ] || { row SKIP reported-sha "no --reported-sha given"; return; }
  local r; r="$(g rev-parse --verify --quiet "$REPORTED^{commit}")"
  if [ -z "$r" ]; then row FAIL reported-sha "'$REPORTED' is not a commit in this repo"; return; fi
  if [ "$r" = "$TIP" ]; then row PASS reported-sha "${r:0:10} is the branch tip"; return; fi
  local where
  if g merge-base --is-ancestor "$r" "$TIP"; then
    where="it is IN the branch, $(g rev-list --count "$r..$TIP") commit(s) behind the tip"
  else
    local brs; brs="$(g branch -a --contains "$r" --format='%(refname:short)' 2>/dev/null | head -n 3 | paste -sd,)"
    if [ -n "$brs" ]; then where="not in $BRANCH; contained in: $brs"
    else where="DANGLING (in no branch; an amended or reset commit?)"; fi
  fi
  row FAIL reported-sha "${r:0:10} != tip ${TIP:0:10}; $where"
}

check_base() {
  local behind; behind="$(g rev-list --count "$TIP..$BASESHA")"
  if g merge-base --is-ancestor "$BASESHA" "$TIP"; then
    row PASS base "$BASE (${BASESHA:0:10}) is an ancestor; behind base by 0"
  else
    row FAIL base "$BASE (${BASESHA:0:10}) is NOT an ancestor; behind base by $behind — rebase the lane"
  fi
}

check_commits() {
  local n; n="$(g rev-list --count "$BASESHA..$TIP")"
  if [ "$n" -eq 0 ]; then row FAIL commits "$BASE..$BRANCH is empty"; return 1; fi
  row PASS commits "$n commit(s) in $BASE..$BRANCH"
  g log --reverse --format='%h %s' "$BASESHA..$TIP" | cut -c1-90 | indent
}

check_trailers() {
  local out
  if out="$(cd "$REPO" && python3 "$HERE/check_commit_trailers.py" --range "$BASESHA..$TIP" 2>&1)"; then
    row PASS trailers "exactly the mandated trailer on every commit"
  else
    row FAIL trailers "check_commit_trailers.py found violations:"
    printf '%s\n' "$out" | indent
  fi
}

check_author() {
  local want="${AUTHOR:-$(g log -1 --format='%an <%ae>' "$BASESHA")}" a bad="" odd=""
  while IFS= read -r a; do
    [ -n "$a" ] || continue
    [ "$a" = "$want" ] && continue
    # A leaked identity: an agent that ran `git config user.*` (CLAUDE.md
    # forbids it) names itself, e.g. "Claude Opus 5 <noreply@…>" (seen on
    # main), or leaves the name or email empty. Only the NAME is tested: the
    # harness default identity itself uses an anthropic address.
    if printf '%s' "${a%% <*}" | grep -qiE 'claude|^ *$' || [[ "$a" == *"<>" ]]; then bad+="$a; "
    else odd+="$a; "; fi
  done < <(g log --format='%an <%ae>' "$BASESHA..$TIP" | sort -u)
  if [ -n "$bad" ]; then row FAIL author "looks like a git-config leak: ${bad%; }"
  elif [ -n "$odd" ]; then row WARN author "differs from expected '$want': ${odd%; }"
  else row PASS author "$want"; fi
}

check_files() {
  FILES="$(g diff --name-only "$MB" "$TIP")"
  row PASS files "$(g diff --shortstat "$MB" "$TIP" | sed 's/^ *//')"
  g diff --stat=100 "$MB" "$TIP" | sed '$d' | head -n 25 | indent
}

check_grant() {
  [ -n "$GRANT" ] || { row SKIP grant "no --grant given"; return; }
  local f glob ok out="" IFS_SAVE="$IFS" globs
  IFS=',' read -ra globs <<< "$GRANT"; IFS="$IFS_SAVE"
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    ok=0
    for glob in "${globs[@]}"; do
      # shellcheck disable=SC2053  # the glob is meant to match as a pattern
      [[ "$f" == $glob ]] && { ok=1; break; }
    done
    [ "$ok" -eq 1 ] || out+="$f"$'\n'
  done <<< "$FILES"
  if [ -z "$out" ]; then row PASS grant "every changed file matches $GRANT"
  else row WARN grant "$(printf '%s' "$out" | grep -c .) file(s) outside $GRANT:"; printf '%s' "$out" | indent; fi
}

check_scripts() {
  # A script that hardcodes an absolute checkout path runs against the wrong
  # tree from any other worktree; ratchet #3413 fails a gate cycle for it.
  local pat='^[^#]*/home/[A-Za-z0-9_.-]+' f hits="" wt untracked=""
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    g show "$TIP:$f" 2>/dev/null | grep -qE "$pat" && hits+="$f "
  done < <(g diff --name-only --diff-filter=AM "$MB" "$TIP" -- 'scripts/*.sh' 'scripts/**/*.sh' 'scripts/hooks/*')
  wt="$(g worktree list --porcelain | awk -v b="refs/heads/$BRANCH" '/^worktree /{w=substr($0,10)} $0=="branch " b {print w}')"
  if [ -n "$wt" ] && [ -d "$wt" ]; then
    while IFS= read -r f; do
      [ -n "$f" ] || continue
      untracked+="$f "
      case "$f" in *.sh|scripts/hooks/*) grep -qE "$pat" "$wt/$f" 2>/dev/null && hits+="$f(untracked) " ;; esac
    done < <(git -C "$wt" ls-files --others --exclude-standard -- scripts/)
  fi
  if [ -n "$hits" ]; then row FAIL scripts "hardcoded absolute /home path (#3413): $hits"
  elif [ -n "$untracked" ]; then row WARN scripts "untracked in $wt (not in the handback): $untracked"
  else row PASS scripts "no new scripts/*.sh or scripts/hooks/* with a hardcoded /home path"; fi
}

check_needles() {
  local out n
  if ! out="$(python3 "$HERE/pregate_stale_needles.py" --repo "$REPO" --from-ref \
                --base "$MB" --head "$TIP" 2>&1)"; then
    row WARN needles "pregate_stale_needles.py did not run cleanly:"; printf '%s\n' "$out" | tail -n 5 | indent; return
  fi
  n="$(printf '%s\n' "$out" | grep -c 'CANDIDATE')"
  if [ "$n" -eq 0 ]; then row PASS needles "$(printf '%s\n' "$out" | head -n 1)"
  else row WARN needles "$n candidate stale needle(s) — a renamed diagnostic?"; printf '%s\n' "$out" | grep CANDIDATE | indent; fi
}

check_named_tests() {
  # Identifiers in the commit messages that look like a gtest `Suite.name` or a
  # tests/ path must exist at the tip; a test named in a message but absent from
  # the tree is a renamed, deleted or never-committed row.
  local msgs id suite name missing="" n=0
  msgs="$(g log --format=%B "$BASESHA..$TIP")"
  while IFS= read -r id; do
    [ -n "$id" ] || continue
    n=$((n + 1))
    if [[ "$id" == tests/* ]]; then
      g cat-file -e "$TIP:$id" 2>/dev/null || \
        g cat-file -e "$TIP:scripts/gate/$id" 2>/dev/null || missing+="$id "
    elif [[ "$id" == scripts/* ]]; then
      g cat-file -e "$TIP:$id" 2>/dev/null || missing+="$id "
    else
      suite="${id%%.*}"; name="${id#*.}"
      g grep -qE "\\b$suite\\b.*\\b$name\\b" "$TIP" -- tests/ scripts/gate/tests/ 2>/dev/null || missing+="$id "
    fi
  done < <( { printf '%s\n' "$msgs" | grep -oE '\btests/[-A-Za-z0-9_./-]+\.(cpp|hpp|bzl|py|tsv)\b'
              printf '%s\n' "$msgs" | grep -oE '\b[A-Z][A-Za-z0-9_]{2,}\.[A-Za-z_][A-Za-z0-9_]{2,}\b' \
                | grep -vE '\.(md|py|sh|cpp|hpp|bzl|txt|tsv|json|log|cmake)$'
            } | sort -u )
  if [ "$n" -eq 0 ]; then row SKIP named-tests "no Suite.name or tests/ path in the commit messages"
  elif [ -z "$missing" ]; then row PASS named-tests "all $n named test(s) found at the tip"
  else row WARN named-tests "named in a commit message but not found at the tip: $missing"; fi
}

check_reverts() {
  # A lane that hits a wall records it and pushes on. A REVERT/UAF/NOT LANDED
  # commit in the branch's own history is therefore a blocking finding the
  # candidate diff cannot show: `git show <sha>` is three-way against <sha>^, so
  # when the revert IS the parent, the candidate reads as a clean forward fix.
  # R28: a reviewer returned PICK on a codegen commit whose own parent was
  # "REVERT fix #NNNN: ... introduces a UAF" recording an ASan heap-use-after-free.
  # Scan the branch's ancestry and make the lane answer why it was reverted.
  local hits="" depth="${HANDBACK_REVERT_DEPTH:-12}"
  # Anchored to the START of the subject for REVERT: a commit that merely
  # *mentions* one ("surface a REVERT/UAF ancestor before a pick" — this
  # file's own history) is not itself a revert. Anchoring still catches the
  # real shape, "REVERT fix #5434: ... introduces a UAF".
  #
  # --skip=1: scan ANCESTORS, not the candidate. The candidate is the thing
  # being judged, and a soundness FIX legitimately names the defect it fixes
  # ("retire #4832's operand release in record .Equals: use-after-free",
  # R28) — matching that would WARN every correct repair of a UAF. The revert
  # that matters is always in the ancestry (R28 #5434: the candidate's own
  # parent was the REVERT).
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    hits+="$line"$'\n'
  done < <(g log --first-parent -n "$depth" --skip=1 --format='%h %s' "$TIP" 2>/dev/null \
             | grep -iE '^([0-9a-f]+ )?[^ ]*(revert|reverted)[^ ]* |not landed|not-landed|superseded by|unsafe:|do not land')
  if [ -z "$hits" ]; then
    row PASS reverts "no revert/UAF marker in the last $depth first-parent commits"
  else
    row WARN reverts "ancestor records a revert or unsoundness — the candidate MUST explain why it is now landable:"
    printf '%s' "$hits" | indent
  fi
}

echo "handback_check: $BRANCH vs $BASE  (repo $REPO)"
if check_branch; then
  check_reported
  check_base
  if check_commits; then
    check_trailers; check_author; check_files; check_grant
    check_scripts; check_needles; check_named_tests; check_reverts
  fi
fi
if [ "$NFAIL" -gt 0 ]; then echo "VERDICT: FAIL ($NFAIL fail, $NWARN warn)"; exit 1; fi
if [ "$NWARN" -gt 0 ]; then echo "VERDICT: PASS-WITH-WARNINGS ($NWARN warn) — read them"; exit 0; fi
echo "VERDICT: PASS"
