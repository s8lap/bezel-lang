#!/usr/bin/env bash
# Reap ORPHANED heavy dev processes — a build/suite/asan_sweep whose launching
# agent died (budget death, kill, crash) leaving a detached `bezel_tests`,
# `asan_sweep.sh`, `run_parallel.sh`, `cc1plus`, `clang`, or `cmake` still
# consuming the box. These are the processes that starve the rolling producer
# pool (lever #6a).
#
# SAFE BY DEFAULT: dry-run — it only LISTS candidates. Pass --kill to signal
# them. It targets ONLY processes that are BOTH (a) a known-heavy dev process
# AND (b) reparented to init (PPID==1) — i.e. genuinely orphaned; a process
# still owned by a live agent shell (PPID!=1) is never touched, so this cannot
# kill an in-flight gate. The match names are DISTINCTIVE substrings (no bare
# `ld`/`lld`/`mold` — those matched "firewalld" and are transient anyway).
#
# Usage:
#   scripts/reap_orphans.sh          # dry-run: list orphaned heavy procs
#   scripts/reap_orphans.sh --kill   # SIGTERM them (then SIGKILL stragglers)
set -u

KILL=0
[ "${1:-}" = "--kill" ] && KILL=1

PATTERN='bezel_tests|cc1plus|asan_sweep\.sh|run_parallel\.sh|clang|cmake'

found=0
# ps: pid, ppid, etime, rss(KiB), full command. Match the command, require the
# process be orphaned (PPID==1).
while read -r pid ppid etime rss cmd; do
  [ "$ppid" = "1" ] || continue
  printf '%s' "$cmd" | grep -qE "$PATTERN" || continue
  found=$((found+1))
  echo "orphan pid=$pid up=$etime rss=${rss}K  $cmd"
  if [ "$KILL" = 1 ]; then
    kill -TERM "$pid" 2>/dev/null && echo "  -> SIGTERM $pid"
  fi
done < <(ps -eo pid=,ppid=,etime=,rss=,args=)

if [ "$found" -eq 0 ]; then
  echo "no orphaned heavy dev processes"
  exit 0
fi

if [ "$KILL" = 1 ]; then
  sleep 2
  while read -r pid ppid cmd; do
    [ "$ppid" = "1" ] || continue
    printf '%s' "$cmd" | grep -qE "$PATTERN" || continue
    kill -KILL "$pid" 2>/dev/null && echo "  -> SIGKILL (straggler) $pid"
  done < <(ps -eo pid=,ppid=,args=)
  echo "reaped orphaned heavy dev processes"
else
  echo
  echo "$found orphaned heavy process(es) above — re-run with --kill to reap"
fi
