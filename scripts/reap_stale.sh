#!/usr/bin/env bash
# reap_stale.sh — reclaim STALE dev processes and waiters, with TWO signals.
#
# WHY THIS EXISTS (R27, 2026-10-06). `reap_orphans.sh` has one signal: PPID==1.
# That misses the dominant case on this box. A lane launches a background wait, the
# lane's AGENT dies, but the shell stays parented to the long-lived `claude`
# session process — NOT to init. Measured, unreaped, 4.25 h old:
#
#   pid=2086422 ppid=2041066   # 2041066 = this session's own `claude`
#   while ! test -f …/beu8znc7i.output || ! grep -q "owningset_take" …;
#   do sleep 2; done
#
# The file existed; the needle never appeared; the waiter could never complete.
#
# So: TWO signals, and the parent signal is the CHEAP one, so it earns a LOWER bar.
#
#   TIER A — OWNERLESS.  parent is init, or the parent PID is gone from /proc.
#              Nothing can ever read this process's output again. Stale at
#              ORPHAN_AGE (default 300s).
#   TIER B — AGED.      parent is alive, whoever it is, but the process has run
#              past AGE (default 1200s). Catches the session-parented waiter.
#
# A signal neither tier has — and the one that makes an aggressive bar safe:
#
#   TIER 0 — WORKING.   CPU time OR disk I/O advanced during the settle window.
#              A slow cold codegen build and a wedged waiter look IDENTICAL on
#              age alone; only progress tells them apart. A waiter burns ~0 CPU
#              and ~0 bytes forever. A build burns CPU, or — when it is blocked
#              in fsync/codegen-write rather than computing — moves
#              /proc/PID/io. So: any candidate making EITHER kind of progress is
#              EXEMPT, at any age, in either tier. This is what lets ORPHAN_AGE
#              be 300s instead of "reap instantly and hope".
#
#              Measured 2026-10-06, and the reason both signals are required: a
#              dead lane's codegen binary read +1 cs CPU on one 3 s window (so it
#              was EXEMPT and left alone) and +0 cs on the next (so it was killed).
#              The kill was correct — the lane was dead — but a CPU-only window
#              that thin cannot tell "wedged" from "blocked on a lock". Reading
#              I/O as well makes the exemption fire far more often, and a build
#              is essentially never at exactly zero progress on BOTH.
#
#   TREE-AWARE (2026-11-05). The exemption is for the whole process TREE: a
#              candidate is exempt when ANY DESCENDANT is working (CPU or I/O
#              advanced) or io-blocked (D). Measured 2026-11-05: of 36 "stale",
#              ~20 were the idle PARENT shells (bash / nohup wrappers) of test
#              suites that live lanes were still running — the bezel_tests
#              child was working and exempt, its parent was not, so --kill
#              would have orphaned live work. A parent waiting on a working
#              child is not wedged.
#
#   PROTECTED  a process whose cwd is inside a PROTECTED worktree is exempt
#              unless --include-protected: the main checkout, the worktree
#              this runs in, every LOCKED worktree (the warm pool, staging),
#              and every path in --protect FILE (one per line; the coordinator
#              passes its live lanes' worktrees). That is where the
#              coordinator's own live waiter loops sit.
#
#   TIER W — WAITER-TARGET-SATISFIED, at any age. A waiter loop whose target
#              is satisfied but which provably cannot see it: its own
#              `pgrep -f X` matches ITSELF (X is in its argv), so
#              `until … ! pgrep -f X` never exits. Decided by
#              scripts/reap_waiter_target.py, at both ends of the settle
#              window, only when the loop parses completely. A loop whose
#              target is satisfied but which is NOT blind (`until grep -q
#              DONE log` with DONE present) is NOT decided — it should exit by
#              itself, and if it does not, the parser is the likelier fault —
#              so it keeps whatever tier its age gives it (A, B or none).
#
# SAFE BY DEFAULT: dry-run. It only LISTS. Pass --kill to signal.
#   - PID-only. Never `pkill` by name (the kill-guard hook refuses it, and a
#     name match would hit an in-flight gate's identically-named binary).
#   - Refuses to touch anything while a gate pidfile is live.
#   - Reports D-state survivors honestly instead of claiming success.
#
# Usage:
#   scripts/reap_stale.sh                 # dry-run: list stale dev procs
#   scripts/reap_stale.sh --age 1800      # Tier B bar   (default 1200s)
#   scripts/reap_stale.sh --orphan-age 60 # Tier A bar   (default  300s)
#   scripts/reap_stale.sh --kill          # SIGTERM, then SIGKILL stragglers
#   scripts/reap_stale.sh --no-exclude-working   # also list working (Tier 0)
#   scripts/reap_stale.sh --protect FILE  # exempt processes cwd'd in these worktrees
#   scripts/reap_stale.sh --include-protected    # do NOT exempt protected worktrees
#   scripts/reap_stale.sh --self-test     # prove the classifier on synthetic input
set -u
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

AGE=1200          # Tier B: seconds alive before an attached process is stale
ORPHAN_AGE=300    # Tier A: seconds alive before an OWNERLESS process is stale
SETTLE=5          # CPU-progress sampling window, seconds
KILL=0
SELF_TEST=0
LIST_WORKING=0
PROTECT_FILE=""
INCLUDE_PROTECTED=0

while [ $# -gt 0 ]; do
  case "$1" in
    --kill)                KILL=1 ;;
    --age)                 AGE="${2:?--age needs a value}"; shift ;;
    --orphan-age)          ORPHAN_AGE="${2:?--orphan-age needs a value}"; shift ;;
    --settle)              SETTLE="${2:?--settle needs a value}"; shift ;;
    --no-exclude-working) LIST_WORKING=1 ;;
    --self-test)           SELF_TEST=1 ;;
    --protect)             PROTECT_FILE="${2:?--protect needs a file}"; shift ;;
    --include-protected)   INCLUDE_PROTECTED=1 ;;
    -h|--help)             sed -n '2,80p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

for pair in "AGE:$AGE" "ORPHAN_AGE:$ORPHAN_AGE" "SETTLE:$SETTLE"; do
  name=${pair%%:*}; val=${pair#*:}
  case "$val" in
    ''|*[!0-9]*) echo "--$name must be a positive integer (got: $val)" >&2; exit 2 ;;
  esac
  [ "$val" -ge 0 ] || { echo "--$name must be >= 0" >&2; exit 2; }
done
[ "$AGE" -ge 30 ]        || { echo "--age must be >= 30s" >&2; exit 2; }
[ "$ORPHAN_AGE" -ge 30 ] || { echo "--orphan-age must be >= 30s" >&2; exit 2; }
if [ -n "$PROTECT_FILE" ] && [ ! -r "$PROTECT_FILE" ]; then
  # A typo must not silently UNPROTECT the coordinator's live lanes.
  echo "--protect file '$PROTECT_FILE' is not readable" >&2; exit 2
fi

# ---------------------------------------------------------------------------
# Classifier. Three families, all observed on this box:
#
#  1. HEAVY DEV — compilers, test binaries, the build/sweep drivers. Bounded
#     work; past the threshold they are wedged, not slow (and the CPU
#     exemption catches the "slow but real" ones regardless).
#  2. WAITER LOOPS — `while ! test -f`, `until … do sleep`, `tail -f`, `wait`.
#     No legitimate multi-hour lifetime: they wait on a log line or a file
#     that a finished lane was going to write and never will.
#  3. SWEEPS — the btrfs-starved test binary: alive for hours, burning no
#     CPU, blocked in commit/fsync retry (D state, `rq_qos_wait`).
#
# `tail -f` is deliberately narrow: only a tail on a file under a scratch or
# worktrees path, so a user's `tail -f /var/log/messages` is never matched.
# ---------------------------------------------------------------------------
HEAVY='bezel_tests_[a-z]+|(^|[ /])bezelc |cc1plus|(^|[ /])clang(\+\+)? |ld\.lld|ld\.mold|cmake --build|dev_heavy\.sh|agent_check\.sh|run_parallel\.sh|asan_sweep\.sh|ratchet_sweep\.sh'
WAITERS='while ! test -f|until .* do sleep|until grep|while (! )?(pgrep|kill -0) |tail -f (/var/tmp/bezel-scratch|/mnt/fast/worktrees|.*bezel-scratch.*)|wait_pid\.sh|chain-half'
PATTERN="$HEAVY|$WAITERS"

# Gate guard: never touch anything while a gate is live. A gate legitimately
# runs bezel_tests for hours, and the pool is zero during one, so age alone
# would misfire. Refuse rather than risk the gate.
GATE_PIDFILE="${BEZEL_GATE_PIDFILE:-}"
if [ -n "$GATE_PIDFILE" ] && [ -f "$GATE_PIDFILE" ]; then
  gpid=$(cat "$GATE_PIDFILE" 2>/dev/null || echo "")
  if [ -n "$gpid" ] && kill -0 "$gpid" 2>/dev/null; then
    echo "reap_stale: a gate is live (pid $gpid from $GATE_PIDFILE) — refusing to reap."
    echo "reap_stale: during a gate the pool is zero and every long process is legitimate."
    exit 0
  fi
  # stale pidfile: verify death before proceeding, per the runbook
  echo "reap_stale: stale gate pidfile ($gpid not alive) — continuing"
fi

# --- shared helpers, also used by the self-test ---------------------------
# cpu_cs <pid> — cumulative CPU in centiseconds, or empty if the pid is gone.
# `ps -o times=` avoids /proc/PID/stat's unquoted-comm field-splitting trap.
cpu_cs() { ps -o times= -p "$1" 2>/dev/null | tr -d ' '; }
# io_bytes <pid> — cumulative bytes read+written, or empty if unreadable.
# Catches a build that is blocked in fsync/codegen-write rather than computing,
# which shows near-zero CPU. /proc/PID/io is the only place this is visible.
io_bytes() {
  local f="/proc/$1/io" r w
  [ -r "$f" ] || { echo ""; return; }
  r=$(awk '/^read_bytes:/ {print $2}' "$f" 2>/dev/null)
  w=$(awk '/^write_bytes:/ {print $2}' "$f" 2>/dev/null)
  [ -n "${r:-}" ] && [ -n "${w:-}" ] && echo $((r + w)) || echo ""
}
# ownerless <ppid> — true when the parent is init or no longer exists.
ownerless() {
  [ "$1" = "1" ] && return 0
  [ -d "/proc/$1" ] && return 1
  return 0
}
# tier_for <ppid> <age> -> A|B|''
tier_for() {
  if ownerless "$1"; then
    [ "$2" -ge "$ORPHAN_AGE" ] && { echo A; return 0; }
  else
    [ "$2" -ge "$AGE" ] && { echo B; return 0; }
  fi
  echo ''
}
# state_of <pid> — the /proc state letter, or ?.
state_of() { awk '{print $3}' "/proc/$1/stat" 2>/dev/null || echo '?'; }

# is_agent_process <cmd> — true when the command line is an opencode AGENT
# process rather than a build.
#
# WHY THIS EXISTS (measured 2026-11-03, the R38 run).  A lane agent is
# launched as `opencode run --session <id> --model ... --agent general "<brief>"`,
# so THE BRIEF IS THE AGENT'S COMMAND-LINE ARGUMENT.  Every lane brief in this
# campaign names `scripts/dev_heavy.sh` and `scripts/agent_check.sh`, so the
# agent's own argv matches HEAVY and the reaper reports the AGENT as a stale
# dev process.  A blanket `reap_stale.sh --kill` would then destroy a LIVE
# lane.  The agent is not wedged: it spends most of its wall-clock waiting on
# the model, so it shows no CPU and no disk I/O, which is exactly the signal
# the reaper uses to call something stale.
#
# ⇒ AN AGENT IS EXEMPT AT ANY AGE.  A lane dies by its SESSION ending, and the
# session reconciler (scripts/pool_lanes.sh) measures that; age is not a
# death certificate for an agent.
#
# The match is deliberately loose (`opencode run` ... `--session` anywhere in
# the line): a false EXEMPTION costs a few hundred wasted bytes, while a false
# REAP costs a lane.  Err toward not killing.
is_agent_process() {
  printf '%s' "$1" | grep -qE '(^|/)opencode run .*--session'
}

# --- process-tree snapshots (the TREE-AWARE exemption) ----------------------
# snap <S0|S1> fills <S>_PP[pid]=ppid, <S>_ST[pid]=state letter,
# <S>_TK[pid]=utime+stime clock ticks, <S>_IO[pid]=read+write bytes (when
# readable). One pass over /proc, builtins only — no fork per process.
declare -A S0_PP=() S0_ST=() S0_TK=() S0_IO=() S1_PP=() S1_ST=() S1_TK=() S1_IO=() KIDS=()
snap() {
  local -n _pp="$1_PP" _st="$1_ST" _tk="$1_TK" _io="$1_IO"
  _pp=(); _st=(); _tk=(); _io=()
  local d p line rest k v r w
  local -a f
  for d in /proc/[0-9]*; do
    p=${d#/proc/}
    { read -r line < "$d/stat"; } 2>/dev/null || continue
    rest=${line##*) }
    read -r -a f <<< "$rest"
    _st[$p]=${f[0]}; _pp[$p]=${f[1]}; _tk[$p]=$(( ${f[11]:-0} + ${f[12]:-0} ))
    r=""; w=""
    { while read -r k v; do
        case $k in read_bytes:) r=$v ;; write_bytes:) w=$v ;; esac
      done < "$d/io"; } 2>/dev/null
    [ -n "$r" ] && [ -n "$w" ] && _io[$p]=$((r + w))
  done
}
# index_kids — KIDS[ppid]="pid pid …" from the S1 snapshot.
index_kids() {
  local p
  KIDS=()
  for p in "${!S1_PP[@]}"; do KIDS[${S1_PP[$p]}]+=" $p"; done
}
# tree_activity <pid> — prints why a DESCENDANT keeps <pid> alive, or nothing:
#   "working <dpid>"  a descendant present in both snapshots advanced CPU or I/O,
#                     or one born inside the window already used >1 tick
#   "blocked <dpid>"  a descendant is in D state (io-blocked: exempt, as for self)
# Err toward not killing: any sign of work below the candidate exempts it.
tree_activity() {
  local -a q=(${KIDS[$1]:-}) seen=()
  local d
  while [ "${#q[@]}" -gt 0 ]; do
    d=${q[0]}; q=("${q[@]:1}")
    case " ${seen[*]:-} " in *" $d "*) continue ;; esac
    seen+=("$d")
    if [ "${S1_ST[$d]:-}" = D ]; then echo "blocked $d"; return 0; fi
    if [ -n "${S0_TK[$d]:-}" ]; then
      if [ "${S1_TK[$d]:-0}" -gt "${S0_TK[$d]}" ]; then echo "working $d"; return 0; fi
      if [ -n "${S0_IO[$d]:-}" ] && [ -n "${S1_IO[$d]:-}" ] && [ "${S1_IO[$d]}" -gt "${S0_IO[$d]}" ]; then
        echo "working $d"; return 0
      fi
    elif [ "${S1_TK[$d]:-0}" -gt 1 ]; then
      echo "working $d"; return 0
    fi
    q+=(${KIDS[$d]:-})
  done
}

# --- the PROTECTED worktree set ------------------------------------------
# canon <path> — realpath without requiring existence; in_tree <path> <root>.
canon() { realpath -m -- "$1" 2>/dev/null || printf '%s\n' "$1"; }
in_tree() { [ "$1" = "$2" ] || [ "${1#"$2"/}" != "$1" ]; }
PROTECTED=()
load_protected() {
  local repo top common l
  repo="$HERE/.."
  top="$(git rev-parse --show-toplevel 2>/dev/null)" && PROTECTED+=("$(canon "$top")")
  common="$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" &&
    PROTECTED+=("$(canon "$(dirname "$common")")")
  while read -r l; do [ -n "$l" ] && PROTECTED+=("$(canon "$l")"); done < <(
    git -C "$repo" worktree list --porcelain 2>/dev/null |
      awk '/^worktree /{p=substr($0,10)} /^locked/{print p}')
  if [ -n "$PROTECT_FILE" ]; then
    while IFS= read -r l || [ -n "$l" ]; do
      l="${l%%#*}"; l="$(printf '%s' "$l" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
      [ -n "$l" ] && PROTECTED+=("$(canon "$l")")
    done < "$PROTECT_FILE"
  fi
}
# protected_root <pid> — the protected worktree holding <pid>'s cwd, or nothing.
protected_root() {
  local c r
  c="$(readlink "/proc/$1/cwd" 2>/dev/null)" || return 0
  for r in "${PROTECTED[@]}"; do in_tree "$c" "$r" && { printf '%s\n' "$r"; return 0; }; done
}
# waiter_verdicts <pid>… — "pid VERDICT" lines from reap_waiter_target.py.
waiter_verdicts() {
  [ "$#" -gt 0 ] || return 0
  python3 "$HERE/reap_waiter_target.py" "$@" 2>/dev/null | awk -F'\t' '{print $1, $2}'
}

# ---------------------------------------------------------------------------
# Self-test: synthetic input, no live processes touched.
# ---------------------------------------------------------------------------
if [ "$SELF_TEST" = 1 ]; then
  fails=0
  assert() { # <desc> <expect: hit|miss|A|B|none> <regex-or-tier> <arg1> <arg2>
    local desc="$1" expect="$2" kind="$3" got
    if [ "$kind" = "tier" ]; then
      got=$(tier_for "$4" "$5"); [ -n "$got" ] || got=none
    else
      if printf '%s' "$desc" | grep -qE "$kind"; then got=hit; else got=miss; fi
    fi
    if [ "$got" = "$expect" ]; then echo "  ok   [$expect] $desc"
    else echo "  FAIL expected=$expect got=$got :: $desc"; fails=$((fails+1)); fi
  }
  assert_reject() { # <desc> <flag>
    if "$0" "$2" abc >/dev/null 2>&1; then
      echo "  FAIL $2 abc should be rejected"; fails=$((fails+1))
    else echo "  ok   [reject] $2 abc"; fi
  }
  assert_reject x --age
  assert_reject x --orphan-age
  assert_reject x --settle
  for f in --age --orphan-age; do
    if "$0" "$f" 5 >/dev/null 2>&1; then
      echo "  FAIL $f 5 should be rejected (below 30s floor)"; fails=$((fails+1))
    else echo "  ok   [reject] $f 5 (below floor)"; fi
  done
  if "$0" --bogus >/dev/null 2>&1; then
    echo "  FAIL --bogus should be rejected"; fails=$((fails+1))
  else echo "  ok   [reject] --bogus"; fi

  echo "self-test: HEAVY/WAITER pattern"
  assert 'build/tests/bezel_tests_codegen --gtest_filter=x'      hit "$PATTERN" "" ""
  assert 'bezelc -c -o /tmp/x.o foo.bzl'                        hit "$PATTERN" "" ""
  assert 'cc1plus -o x.o x.cpp'                                 hit "$PATTERN" "" ""
  assert 'while ! test -f /tmp/x || ! grep -q y /tmp/x; do sleep 2; done' hit "$PATTERN" "" ""
  assert 'tail -f /var/tmp/bezel-scratch/r27/verify/5413.log'   hit "$PATTERN" "" ""
  assert 'tail -f /mnt/fast/worktrees/a24-wt-5305/build.log'    hit "$PATTERN" "" ""
  assert 'agent_check.sh --area codegen foo'                    hit "$PATTERN" "" ""
  assert 'wait_pid.sh /var/tmp/bezel-scratch/gate.pidfile'      hit "$PATTERN" "" ""
  # these MUST NOT match — a false positive here could kill a live agent
  assert 'vim src/main.cpp'                                    miss "$PATTERN" "" ""
  assert 'gnome-terminal --geometry=80x24'                      miss "$PATTERN" "" ""
  assert 'tail -f /var/log/messages'                            miss "$PATTERN" "" ""
  assert 'python3 ~/.claude/hooks/bezel_build_guard.py --root /x' miss "$PATTERN" "" ""
  assert 'git commit -m wip'                                    miss "$PATTERN" "" ""

  echo "self-test: the AGENT exemption (an agent's argv IS its brief)"
  assert_agent() { # <cmd> <expect: exempt|keep>
    local got
    if is_agent_process "$1"; then got=exempt; else got=keep; fi
    if [ "$got" = "$2" ]; then echo "  ok   [$2] ${1:0:72}"
    else echo "  FAIL expected=$2 got=$got :: ${1:0:72}"; fails=$((fails+1)); fi
  }
  # A lane agent: its BRIEF is its argv, so a brief naming dev_heavy.sh or
  # agent_check.sh makes the agent's own command line match HEAVY.  It must be
  # EXEMPT -- reaping it would kill a live lane.
  assert_agent 'opencode run --session ses_x --model minimax/MiniMax-M3.1-Flash --agent general <brief>' exempt
  assert_agent '/home/stepan/.opencode/bin/opencode run --session ses_x --model m --agent general <brief: route every build through scripts/dev_heavy.sh>' exempt
  # Real builds must NOT be exempted.
  assert_agent 'cmake --build build --target bezel_tests -j16' keep
  assert_agent 'scripts/dev_heavy.sh build cmake --build build --target bezelc' keep
  assert_agent 'build/tests/bezel_tests_codegen --gtest_filter=x' keep
  assert_agent 'agent_check.sh --area codegen foo' keep

  echo "self-test: tiering (parent signal sets the LOWER bar)"
  # Ownerless (ppid=1) crosses at ORPHAN_AGE=300, not AGE=1200.
  assert 'ownerless 300s  -> Tier A (orphaned early)'        A     tier 1   300
  assert 'ownerless 299s  -> not yet (under ORPHAN_AGE)'     none  tier 1   299
  # Attached to a live parent: needs the full AGE.
  assert 'attached 1199s  -> not yet (under AGE)'            none  tier $$  1199
  assert 'attached 1200s  -> Tier B'                         B     tier $$  1200
  # ppid 999999 = no such process -> ownerless, early bar.
  assert 'ppid gone  400s  -> Tier A'                        A     tier 999999 400

  echo "self-test: progress probe (the Tier 0 exemption inputs)"
  # cpu_cs/io_bytes must report for a live pid and stay silent for a dead one —
  # a probe that returns a number for a reaped pid would reclassify it as working
  # and silently skip it forever.
  self_c=$(cpu_cs $$); self_i=$(io_bytes $$)
  if [ -n "$self_c" ] && [ -n "$self_i" ]; then
    echo "  ok   [live] cpu_cs/io_bytes report values for a live pid ($$)"
  else
    echo "  FAIL cpu_cs/io_bytes silent for a LIVE pid ($$): cpu='$self_c' io='$self_i'"
    fails=$((fails+1))
  fi
  if [ -z "$(cpu_cs 999999)" ] && [ -z "$(io_bytes 999999)" ]; then
    echo "  ok   [dead] cpu_cs/io_bytes silent for a nonexistent pid"
  else
    echo "  FAIL cpu_cs/io_bytes returned a value for pid 999999"
    fails=$((fails+1))
  fi
  # A self-consuming loop must register as progress over a settle window —
  # this is the exact property that distinguishes a build from a waiter.
  ( exec 3>&1; while :; do printf 'x' >&3; done ) >/dev/null 2>&1 &
  spin=$!
  # 3 s, not 1: `ps -o times` counts WHOLE seconds, so on a loaded box (load
  # ~28, measured 2026-11-05) a 1 s window often read 0 -> 0 (2 of 4 runs of
  # the unchanged script) — a flaky self-test, not a classifier fault.
  s0=$(cpu_cs "$spin"); sleep 3; s1=$(cpu_cs "$spin")
  kill -KILL "$spin" 2>/dev/null; wait "$spin" 2>/dev/null
  if [ -n "$s0" ] && [ -n "$s1" ] && [ "$s1" -gt "$s0" ]; then
    echo "  ok   [progress] a CPU-burning process advances cpu_cs ($s0 -> $s1)"
  else
    echo "  FAIL cpu_cs did not advance for a CPU-burning process ($s0 -> $s1)"
    fails=$((fails+1))
  fi
  # A pure waiter: alive, pattern-matching, but burning nothing. This is what the
  # exemption must NOT rescue, or the whole tiering is useless.
  ( while :; do sleep 1; done ) >/dev/null 2>&1 &
  wtr=$!
  w0=$(cpu_cs "$wtr"); sleep 2; w1=$(cpu_cs "$wtr")
  kill -KILL "$wtr" 2>/dev/null; wait "$wtr" 2>/dev/null
  if [ -n "$w0" ] && [ -n "$w1" ] && [ "$w1" -eq "$w0" ]; then
    echo "  ok   [wedged] a sleep-loop waiter does NOT advance cpu_cs ($w0 -> $w1)"
  else
    echo "  FAIL a sleep-loop waiter advanced cpu_cs ($w0 -> $w1) — exemption would rescue waiters"
    fails=$((fails+1))
  fi

  echo "self-test: TREE-AWARE exemption (an idle parent of a working child)"
  # Measured 2026-11-05: the idle bash/nohup PARENT of a working bezel_tests was
  # listed STALE while the child was exempt. The parent here only `wait`s.
  bash -c 'while :; do :; done & wait' >/dev/null 2>&1 &
  busy_parent=$!
  bash -c 'sleep 300 & wait' >/dev/null 2>&1 &
  idle_parent=$!
  sleep 0.5
  snap S0; sleep 1; snap S1; index_kids
  ta_busy="$(tree_activity "$busy_parent")"; ta_idle="$(tree_activity "$idle_parent")"
  pkill -KILL -P "$busy_parent" 2>/dev/null; pkill -KILL -P "$idle_parent" 2>/dev/null
  kill -KILL "$busy_parent" "$idle_parent" 2>/dev/null; wait "$busy_parent" "$idle_parent" 2>/dev/null
  case "$ta_busy" in
    working*) echo "  ok   [exempt] idle parent of a CPU-burning child ($ta_busy)" ;;
    *) echo "  FAIL idle parent of a working child not exempted (got '$ta_busy')"; fails=$((fails+1)) ;;
  esac
  if [ -z "$ta_idle" ]; then echo "  ok   [keep]   idle parent of an idle (sleep) child is NOT exempted"
  else echo "  FAIL idle parent of a sleep child exempted ($ta_idle)"; fails=$((fails+1)); fi

  echo "self-test: tier W (waiter-target-satisfied)"
  assert 'while pgrep -f x >/dev/null; do sleep 1; done'        hit "$PATTERN" "" ""
  assert 'while kill -0 1234 2>/dev/null; do sleep 60; done'    hit "$PATTERN" "" ""
  # A self-matching waiter: the marker is in its own argv, and nothing else
  # runs it, so `while pgrep -f MARKER` can never exit. Decided SATISFIED.
  mk="reap-selftest-marker-$$-$RANDOM"
  bash -c "while pgrep -f $mk >/dev/null; do sleep 30; done" >/dev/null 2>&1 &
  w_blind=$!
  # Waiting for a file that does not exist: legitimately WAITING.
  bash -c "until test -f /nonexistent/w-$$-$RANDOM; do sleep 30; done" >/dev/null 2>&1 &
  w_wait=$!
  # Unparseable target ($(...) substitution): UNDECIDABLE, left to the age tiers.
  bash -c "until test -n \"\$(cat /nonexistent/u-$$ 2>/dev/null)\"; do sleep 30; done" >/dev/null 2>&1 &
  w_undec=$!
  sleep 1.5
  declare -A WV=()
  while read -r _p _v; do WV[$_p]="$_v"; done < <(waiter_verdicts "$w_blind" "$w_wait" "$w_undec")
  for _p in "$w_blind" "$w_wait" "$w_undec"; do pkill -KILL -P "$_p" 2>/dev/null; kill -KILL "$_p" 2>/dev/null; done
  wait "$w_blind" "$w_wait" "$w_undec" 2>/dev/null
  for _c in "$w_blind:SATISFIED:self-matching pgrep waiter, target gone" \
            "$w_wait:WAITING:until test -f <missing file>" \
            "$w_undec:UNDECIDABLE:command substitution in the condition"; do
    _p=${_c%%:*}; _r=${_c#*:}; _want=${_r%%:*}; _d=${_r#*:}
    if [ "${WV[$_p]:-}" = "$_want" ]; then echo "  ok   [$_want] $_d"
    else echo "  FAIL expected=$_want got=${WV[$_p]:-none} :: $_d"; fails=$((fails+1)); fi
  done

  if [ "$fails" -eq 0 ]; then echo "self-test: PASS"; exit 0; fi
  echo "self-test: FAIL ($fails assertion(s))"; exit 1
fi

# ---------------------------------------------------------------------------
# Pass 1: collect candidates (pattern + tier), sampling CPU at the same time.
# A WAITER-pattern process under both age bars is not a tier candidate, but is
# kept as a W-probe: tier W can decide it at any age.
# ---------------------------------------------------------------------------
declare -A CPU0=() IO0=()
declare -a C_PID=() C_TIER=() C_AGE=() C_CMD=() PW_PID=()
n_tiered=0 n_wprobe=0 n_protected=0
load_protected

while read -r pid ppid etimes rss cmd; do
  [ -n "${pid:-}" ] || continue
  [ "$pid" = "$$" ] && continue
  [ "$pid" = "$PPID" ] && continue
  [ "$etimes" -ge 30 ] 2>/dev/null || continue
  printf '%s' "$cmd" | grep -qE "$PATTERN" || continue
  # Off-limits regardless of age: an in-flight gate even if the pidfile is absent.
  printf '%s' "$cmd" | grep -q 'gate_and_push' && continue
  # Off-limits regardless of age: a LANE AGENT (see is_agent_process). Its
  # argv carries the brief, so a brief naming dev_heavy.sh/agent_check.sh
  # makes the agent look like a build; reaping it would kill a live lane.
  is_agent_process "$cmd" && continue
  t=$(tier_for "$ppid" "$etimes")
  if [ -z "$t" ]; then
    printf '%s' "$cmd" | grep -qE "$WAITERS" || continue
    t=-   # W-probe only
  fi
  # Off-limits unless --include-protected: cwd inside a PROTECTED worktree.
  if [ "$INCLUDE_PROTECTED" = 0 ] && [ -n "$(protected_root "$pid")" ]; then
    [ "$t" != - ] && n_protected=$((n_protected+1))
    # still DECIDED for tier W, so the summary can say which protected waiters
    # are provably wedged (reaped only under --include-protected)
    printf '%s' "$cmd" | grep -qE "$WAITERS" && PW_PID+=("$pid")
    [ "$LIST_WORKING" = 1 ] && [ "$t" != - ] && printf 'protected pid=%s age=%ss cwd in %s  %s\n' \
      "$pid" "$etimes" "$(protected_root "$pid")" "$(printf '%s' "$cmd" | cut -c1-80)"
    continue
  fi
  if [ "$t" = - ]; then n_wprobe=$((n_wprobe+1)); else n_tiered=$((n_tiered+1)); fi
  C_PID+=("$pid"); C_TIER+=("$t"); C_AGE+=("$etimes"); C_CMD+=("$cmd")
  CPU0["$pid"]=$(cpu_cs "$pid"); IO0["$pid"]=$(io_bytes "$pid")
done < <(ps -eo pid=,ppid=,etimes=,rss=,args=)

if [ "${#C_PID[@]}" -eq 0 ] && [ "${#PW_PID[@]}" -eq 0 ]; then
  echo "no stale dev processes (tier A >= ${ORPHAN_AGE}s ownerless, tier B >= ${AGE}s attached, tier W any age; $n_protected protected exempt)"
  exit 0
fi

# Tier W, first look: which waiters have a SATISFIED target right now.
declare -A W0=() W1=()
mapfile -t _wp < <(for i in "${!C_PID[@]}"; do
  printf '%s' "${C_CMD[$i]}" | grep -qE "$WAITERS" && echo "${C_PID[$i]}"; done
  for _p in "${PW_PID[@]}"; do echo "$_p"; done)
while read -r _p _v; do [ -n "${_p:-}" ] && W0[$_p]="$_v"; done < <(waiter_verdicts "${_wp[@]}")
snap S0

# ---------------------------------------------------------------------------
# Pass 2: progress exemption (Tier 0). A candidate whose CPU OR disk I/O advanced
# across the settle window is WORKING — a slow build, not a wedge. Exempt at any
# age. This is the signal that makes the aggressive bars safe. Requiring BOTH
# signals to be still before reaping is deliberate: the cost of a missed reap is
# a few hundred wasted NVMe bytes, the cost of a false reap is a dead lane.
# The same holds for the candidate's whole TREE (tree_activity above).
# ---------------------------------------------------------------------------
sleep "$SETTLE"
snap S1
index_kids
while read -r _p _v; do [ -n "${_p:-}" ] && W1[$_p]="$_v"; done < <(waiter_verdicts "${_wp[@]}")
declare -a K_PID=() K_TIER=() K_AGE=() K_CMD=() K_D=()
working=0 tree_exempt=0
for i in "${!C_PID[@]}"; do
  pid=${C_PID[$i]}
  if ! kill -0 "$pid" 2>/dev/null; then continue; fi   # died on its own: goal met
  c0=${CPU0[$pid]:-0}; c1=$(cpu_cs "$pid")
  [ -n "$c1" ] || continue
  i0=${IO0[$pid]:-0};  i1=$(io_bytes "$pid"); [ -n "$i1" ] || i1="$i0"
  dcpu=$((c1 - c0)); dio=$((i1 - i0))
  if [ "$dcpu" -gt 0 ] || [ "$dio" -gt 0 ]; then
    [ "${C_TIER[$i]}" = - ] && continue
    working=$((working+1))
    [ "$LIST_WORKING" = 1 ] && printf 'working pid=%s age=%ss (+%s cs cpu, +%s B io)  %s\n' \
      "$pid" "${C_AGE[$i]}" "$dcpu" "$dio" "$(printf '%s' "${C_CMD[$i]}" | cut -c1-80)"
    continue
  fi
  # D state is an EXEMPTION, not a reap target. A process in uninterruptible sleep
  # is by definition blocked on real I/O, so "no progress" is the expected
  # condition and carries no information about whether it is wedged. Signalling
  # it is also futile: SIGTERM and SIGKILL both land in the kernel's pending
  # set and it stays put until the I/O completes. R27 measured a btrfs-transaction
  # kernel thread sitting in D for 185841 s (51 h) — reporting that as a failed
  # reap would be a false claim, and reaping it would be theatre.
  #
  # This is the io-throttled case the progress probe cannot separate: a genuinely
  # working build under heavy btrfs throttle shows dcpu=0 AND dio=0, exactly like
  # a stuck waiter. D state is the only reliable discriminator, so it exempts.
  if [ "$(state_of "$pid")" = "D" ]; then
    [ "${C_TIER[$i]}" = - ] && continue
    K_D+=("$pid")
    printf 'BLOCKED pid=%s age=%ss state=D wchan=%s — io-throttled, NOT reapable\n' \
      "$pid" "${C_AGE[$i]}" "$(cat "/proc/$pid/wchan" 2>/dev/null || echo '?')"
    continue
  fi
  # TREE-AWARE: an idle parent of a working or io-blocked descendant is exempt.
  ta="$(tree_activity "$pid")"
  if [ -n "$ta" ]; then
    [ "${C_TIER[$i]}" = - ] && continue
    tree_exempt=$((tree_exempt+1))
    [ "$LIST_WORKING" = 1 ] && printf 'working-tree pid=%s age=%ss (descendant %s)  %s\n' \
      "$pid" "${C_AGE[$i]}" "$ta" "$(printf '%s' "${C_CMD[$i]}" | cut -c1-80)"
    continue
  fi
  tier=${C_TIER[$i]}
  # Tier W: decided SATISFIED at BOTH ends of the settle window.
  if [ "${W0[$pid]:-}" = SATISFIED ] && [ "${W1[$pid]:-}" = SATISFIED ]; then
    tier=W
  fi
  [ "$tier" = - ] && continue
  K_PID+=("$pid"); K_TIER+=("$tier"); K_AGE+=("${C_AGE[$i]}"); K_CMD+=("${C_CMD[$i]}")
done

n_w=0; for t in "${K_TIER[@]}"; do [ "$t" = W ] && n_w=$((n_w+1)); done
echo "candidates: $n_tiered  |  working (exempt): $working  |  working-tree (exempt): $tree_exempt  |  io-blocked (exempt): ${#K_D[@]}  |  protected (exempt): $n_protected  |  waiter-probed: $n_wprobe  |  stale: ${#K_PID[@]} (tier W: $n_w)"
_pw=""; for _p in "${PW_PID[@]}"; do
  [ "${W0[$_p]:-}" = SATISFIED ] && [ "${W1[$_p]:-}" = SATISFIED ] && kill -0 "$_p" 2>/dev/null && _pw="$_pw $_p"
done
[ -n "$_pw" ] && echo "  protected waiters decided tier W (provably wedged; reaped only with --include-protected):$_pw"
[ "${#K_D[@]}" -gt 0 ] && \
  echo "  io-blocked need FILESYSTEM relief (btrfs writeback), not a signal — see feedback-btrfs-d-state-starves-test-harness"
[ $((working + tree_exempt + n_protected)) -gt 0 ] && [ "$LIST_WORKING" = 0 ] && \
  echo "  ($working working, $tree_exempt with a working/io-blocked descendant, $n_protected in a protected worktree — use --no-exclude-working to see them)"

if [ "${#K_PID[@]}" -eq 0 ]; then
  echo "nothing stale: every candidate is still making progress (itself or its tree), protected, or undecided"
  exit 0
fi

for i in "${!K_PID[@]}"; do
  pid=${K_PID[$i]}; tier=${K_TIER[$i]}; age=${K_AGE[$i]}
  st=$(state_of "$pid")
  _why=""; [ "$tier" = W ] && _why=" reason=waiter-target-satisfied"
  printf 'STALE tier=%s pid=%s age=%ss (%dm) state=%s%s  %s\n' "$tier" "$pid" "$age" \
    "$((age/60))" "$st" "$_why" "$(printf '%s' "${K_CMD[$i]}" | cut -c1-100)"
  if [ "$KILL" = 1 ]; then
    if [ "$st" = "D" ]; then
      echo "  -> $pid is in D state (wchan=$(cat "/proc/$pid/wchan" 2>/dev/null || echo ?))"
      echo "     NOT signalable — relieve FILESYSTEM pressure (btrfs writeback), not the process."
    else
      kill -TERM "$pid" 2>/dev/null && echo "  -> SIGTERM $pid"
    fi
  fi
done

if [ "$KILL" = 1 ]; then
  sleep 2
  remaining=0; killed=0
  for i in "${!K_PID[@]}"; do
    pid=${K_PID[$i]}
    kill -0 "$pid" 2>/dev/null || { killed=$((killed+1)); continue; }
    if [ "$(state_of "$pid")" = "D" ]; then
      echo "  -> $pid survives in D state — needs filesystem relief, see"
      echo "     feedback-btrfs-d-state-starves-test-harness"
    else
      kill -KILL "$pid" 2>/dev/null && { killed=$((killed+1)); echo "  -> SIGKILL $pid"; }
    fi
    kill -0 "$pid" 2>/dev/null && remaining=$((remaining+1))
  done
  echo "reaped $killed/${#K_PID[@]} stale process(es)$([ "$remaining" -gt 0 ] && echo " ($remaining need filesystem relief)")"
else
  echo
  echo "${#K_PID[@]} stale process(es) above — re-run with --kill to reap"
fi
