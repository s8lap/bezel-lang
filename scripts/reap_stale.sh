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
#   scripts/reap_stale.sh --self-test     # prove the classifier on synthetic input
set -u

AGE=1200          # Tier B: seconds alive before an attached process is stale
ORPHAN_AGE=300    # Tier A: seconds alive before an OWNERLESS process is stale
SETTLE=5          # CPU-progress sampling window, seconds
KILL=0
SELF_TEST=0
LIST_WORKING=0

while [ $# -gt 0 ]; do
  case "$1" in
    --kill)                KILL=1 ;;
    --age)                 AGE="${2:?--age needs a value}"; shift ;;
    --orphan-age)          ORPHAN_AGE="${2:?--orphan-age needs a value}"; shift ;;
    --settle)              SETTLE="${2:?--settle needs a value}"; shift ;;
    --no-exclude-working) LIST_WORKING=1 ;;
    --self-test)           SELF_TEST=1 ;;
    -h|--help)             sed -n '2,45p' "$0"; exit 0 ;;
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
WAITERS='while ! test -f|until .* do sleep|until grep|tail -f (/var/tmp/bezel-scratch|/mnt/fast/worktrees|.*bezel-scratch.*)|wait_pid\.sh|chain-half'
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
  s0=$(cpu_cs "$spin"); sleep 1; s1=$(cpu_cs "$spin")
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

  if [ "$fails" -eq 0 ]; then echo "self-test: PASS"; exit 0; fi
  echo "self-test: FAIL ($fails assertion(s))"; exit 1
fi

# ---------------------------------------------------------------------------
# Pass 1: collect candidates (pattern + tier), sampling CPU at the same time.
# ---------------------------------------------------------------------------
declare -A CPU0=() IO0=()
declare -a C_PID=() C_TIER=() C_AGE=() C_CMD=()

while read -r pid ppid etimes rss cmd; do
  [ -n "${pid:-}" ] || continue
  [ "$pid" = "$$" ] && continue
  [ "$pid" = "$PPID" ] && continue
  [ "$etimes" -ge 30 ] 2>/dev/null || continue
  printf '%s' "$cmd" | grep -qE "$PATTERN" || continue
  # Off-limits regardless of age: an in-flight gate even if the pidfile is absent.
  printf '%s' "$cmd" | grep -q 'gate_and_push' && continue
  t=$(tier_for "$ppid" "$etimes")
  [ -n "$t" ] || continue
  C_PID+=("$pid"); C_TIER+=("$t"); C_AGE+=("$etimes"); C_CMD+=("$cmd")
  CPU0["$pid"]=$(cpu_cs "$pid"); IO0["$pid"]=$(io_bytes "$pid")
done < <(ps -eo pid=,ppid=,etimes=,rss=,args=)

if [ "${#C_PID[@]}" -eq 0 ]; then
  echo "no stale dev processes (tier A >= ${ORPHAN_AGE}s ownerless, tier B >= ${AGE}s attached)"
  exit 0
fi

# ---------------------------------------------------------------------------
# Pass 2: progress exemption (Tier 0). A candidate whose CPU OR disk I/O advanced
# across the settle window is WORKING — a slow build, not a wedge. Exempt at any
# age. This is the signal that makes the aggressive bars safe. Requiring BOTH
# signals to be still before reaping is deliberate: the cost of a missed reap is
# a few hundred wasted NVMe bytes, the cost of a false reap is a dead lane.
# ---------------------------------------------------------------------------
sleep "$SETTLE"
declare -a K_PID=() K_TIER=() K_AGE=() K_CMD=()
working=0
for i in "${!C_PID[@]}"; do
  pid=${C_PID[$i]}
  if ! kill -0 "$pid" 2>/dev/null; then continue; fi   # died on its own: goal met
  c0=${CPU0[$pid]:-0}; c1=$(cpu_cs "$pid")
  [ -n "$c1" ] || continue
  i0=${IO0[$pid]:-0};  i1=$(io_bytes "$pid"); [ -n "$i1" ] || i1="$i0"
  dcpu=$((c1 - c0)); dio=$((i1 - i0))
  if [ "$dcpu" -gt 0 ] || [ "$dio" -gt 0 ]; then
    working=$((working+1))
    [ "$LIST_WORKING" = 1 ] && printf 'working pid=%s age=%ss (+%s cs cpu, +%s B io)  %s\n' \
      "$pid" "${C_AGE[$i]}" "$dcpu" "$dio" "$(printf '%s' "${C_CMD[$i]}" | cut -c1-80)"
    continue
  fi
  K_PID+=("$pid"); K_TIER+=("${C_TIER[$i]}"); K_AGE+=("${C_AGE[$i]}"); K_CMD+=("${C_CMD[$i]}")
done

echo "candidates: ${#C_PID[@]}  |  working (exempt): $working  |  stale: ${#K_PID[@]}"
[ "$working" -gt 0 ] && [ "$LIST_WORKING" = 0 ] && \
  echo "  ($working skipped as genuinely in progress — use --no-exclude-working to see them)"

if [ "${#K_PID[@]}" -eq 0 ]; then
  echo "nothing stale: every candidate is still making progress"
  exit 0
fi

for i in "${!K_PID[@]}"; do
  pid=${K_PID[$i]}; tier=${K_TIER[$i]}; age=${K_AGE[$i]}
  st=$(state_of "$pid")
  printf 'STALE tier=%s pid=%s age=%ss (%dm) state=%s  %s\n' "$tier" "$pid" "$age" \
    "$((age/60))" "$st" "$(printf '%s' "${K_CMD[$i]}" | cut -c1-100)"
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
