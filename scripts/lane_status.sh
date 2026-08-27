#!/usr/bin/env bash
# lane_status.sh — one-command heartbeat view of in-flight fix lanes.
#
# WHY: the coordinator's periodic check used to be 4-6 ad-hoc commands per lane
# (git log / status / ps greps), and got two things wrong repeatedly:
#   * stall detection keyed on the AGENT ID — pool builds don't carry the id in
#     their cmdline, so a busy lane read as dead. Key on the worktree PATH.
#   * liveness greps watched `cc1plus`/`ninja` — this project builds with clang
#     via ninja-build, so those patterns read zero on a fully busy box.
# Both are baked in here.
#
# Usage:
#   scripts/lane_status.sh                 # auto: worktrees touched in the last 4h
#   scripts/lane_status.sh <wt> [<wt>...]  # explicit worktree paths (or basenames)
#   BEZEL_LANE_WINDOW_MIN=240              # override the auto-detect window
#
# Columns: worktree | branch | +commits (vs origin/main) | dirty | idle (index mtime)
#          | PROC (live build/test procs rooted in the tree) | last commit subject
set -u

# fix #3413: derive the root, never fall back to a literal. The fallback here
# was `|| echo /home/stepan/w/code/bezel-lang` — the same shape that made
# gate_full_cycle.sh report on a tree the operator never selected, and the same
# reason it is wrong: a script that cannot find its repo must say so, not
# silently report on somebody else's checkout.
ROOT="$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel 2>/dev/null)" || ROOT=""
if [ -z "$ROOT" ]; then
  echo "lane_status: cannot derive a repo root from $(dirname "$0")/.. — refusing to guess (#3413)." >&2
  exit 2
fi
WINDOW_MIN="${BEZEL_LANE_WINDOW_MIN:-240}"
now=$(date +%s)

# --- collect candidate worktrees -------------------------------------------------
declare -a WTS=()
if [ "$#" -gt 0 ]; then
  for a in "$@"; do
    if [ -d "$a" ]; then WTS+=("$a")
    elif [ -d "$ROOT/.claude/worktrees/$a" ]; then WTS+=("$ROOT/.claude/worktrees/$a")
    else echo "[lane_status] no such worktree: $a" >&2; fi
  done
else
  while read -r wt; do
    [ "$wt" = "$ROOT" ] && continue
    idx="$wt/.git"
    [ -f "$idx" ] && idx="$(sed -n 's/^gitdir: //p' "$wt/.git")/index"
    [ -d "$wt/.git" ] && idx="$wt/.git/index"
    [ -f "$idx" ] || continue
    age_min=$(( (now - $(stat -c %Y "$idx" 2>/dev/null || echo 0)) / 60 ))
    [ "$age_min" -le "$WINDOW_MIN" ] && WTS+=("$wt")
  done < <(git -C "$ROOT" worktree list --porcelain | sed -n 's/^worktree //p')
fi

if [ "${#WTS[@]}" -eq 0 ]; then
  echo "[lane_status] no lane worktrees active in the last ${WINDOW_MIN}m"
  exit 0
fi

# --- one pass over /proc: cwd + cmdline of every live process --------------------
# PROC detection is two-pronged, because either alone has a blind spot:
#   (a) a process cwd'd inside the tree (covers cmake/ninja/agent_check shells)
#   (b) a build/test process whose CMDLINE mentions the tree (covers clang++
#       invoked from elsewhere with absolute paths, and detached test runs)
declare -A CWDS=()
for p in /proc/[0-9]*; do
  c="$(readlink "$p/cwd" 2>/dev/null)" || continue
  [ -n "$c" ] && CWDS["$c"]=1
done

proc_hits() { # $1 = worktree path -> count of live build/test procs attributable to it
  # ⚠ lane_setup.sh symlinks <wt>/build -> /mnt/fast/lane-builds/<name>, and a
  # compiler's cwd resolves through the symlink, so matching the worktree path
  # ALONE reports zero on a fully busy lane. Match the NVMe build dir too.
  local wt="$1" n=0
  local blink; blink="$(readlink -f "$wt/build" 2>/dev/null)"
  for c in "${!CWDS[@]}"; do
    case "$c" in "$wt"*) n=$((n+1));; esac
    [ -n "$blink" ] && case "$c" in "$blink"*) n=$((n+1));; esac
  done
  # cmdline fallback: ccache/clang++/gmake/ninja/test binaries naming either path.
  local m
  m=$(pgrep -a -f 'ccache|clang\+\+|cc1plus|ninja|gmake|make|cmake|bezel_tests|bezelc' 2>/dev/null \
        | grep -cE -- "$wt|${blink:-__none__}" || true)
  echo $(( n > m ? n : m ))
}

printf '%-26s %-26s %4s %5s %6s %5s  %s\n' WORKTREE BRANCH "+CMT" DIRTY IDLEm PROC LAST-COMMIT
for wt in "${WTS[@]}"; do
  name="$(basename "$wt")"
  br="$(git -C "$wt" branch --show-current 2>/dev/null)"; [ -z "$br" ] && br='(detached)'
  ahead="$(git -C "$wt" rev-list --count origin/main..HEAD 2>/dev/null || echo '?')"
  dirty="$(git -C "$wt" status --porcelain 2>/dev/null | wc -l)"
  idx="$wt/.git/index"
  [ -f "$wt/.git" ] && idx="$(sed -n 's/^gitdir: //p' "$wt/.git")/index"
  idle=$(( (now - $(stat -c %Y "$idx" 2>/dev/null || echo "$now")) / 60 ))
  procs="$(proc_hits "$wt")"
  subj="$(git -C "$wt" log -1 --format='%s' 2>/dev/null | cut -c1-64)"
  flag=''
  # A lane with no live process AND an untouched index for >20m is a stall candidate.
  [ "$procs" -eq 0 ] && [ "$idle" -ge 20 ] && flag=' <-- STALL?'
  printf '%-26s %-26s %4s %5s %6s %5s  %s%s\n' \
    "${name:0:26}" "${br:0:26}" "$ahead" "$dirty" "$idle" "$procs" "$subj" "$flag"
done

echo
echo "-- box: load$(uptime | sed 's/.*load average//') | mem free $(free -g | awk 'NR==2{print $7}')G | heavy procs: $(pgrep -c -f 'clang\+\+|ninja-build|bezel_tests' 2>/dev/null || echo 0)"
