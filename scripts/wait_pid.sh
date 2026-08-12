#!/usr/bin/env bash
# wait_pid.sh — wait for a long-running job WITHOUT pattern-matching process lists.
#
# WHY THIS EXISTS. Twice in the 2026-08-12 run a coordinator wait-loop of the form
#
#     until ! pgrep -f 'cmake --build build' >/dev/null; do sleep 10; done
#
# never terminated, because the waiter's OWN command line contains the literal
# pattern (it is the argument to pgrep), so pgrep matched the waiter itself. One
# instance span for ~12 hours over a build that had finished in 9 minutes, and
# every "is it still running?" check made against the same pattern returned a
# false positive from the zombie. Measured cost: ~2.2 h, about 13% of that run.
#
# Bracketing the pattern (`[c]make`) is NOT a general fix: it defeats a self-match
# only when the pattern appears solely as grep's argument. If the waiter's command
# line carries the target string for any other reason — a log path, an echo, a
# second grep — the bracketed form matches it too. The 2026-08-12 gate waiters
# failed exactly that way.
#
# The rule this script encodes: WAIT ON IDENTITY, NEVER ON A PATTERN.
#
# Usage:
#   scripts/wait_pid.sh <pidfile> [--timeout SECONDS] [--poll SECONDS]
#   scripts/wait_pid.sh --pid <pid>  [--timeout SECONDS] [--poll SECONDS]
#
# Exit: 0 when the process is gone; 2 on timeout; 3 on a missing/garbage pidfile.
#
# NOTE: prefer the harness's own background-job tracking where available — a job
# started with run_in_background notifies on completion and needs no waiter at
# all. This script is for jobs launched detached (nohup ... &), where the pidfile
# is the only identity handle.
set -uo pipefail

PIDFILE=""; PID=""; TIMEOUT=0; POLL=10

while [ $# -gt 0 ]; do
  case "$1" in
    --pid)     PID="${2:-}"; shift 2 ;;
    --timeout) TIMEOUT="${2:-0}"; shift 2 ;;
    --poll)    POLL="${2:-10}"; shift 2 ;;
    -h|--help) sed -n '1,40p' "$0"; exit 0 ;;
    *)         PIDFILE="$1"; shift ;;
  esac
done

if [ -z "$PID" ]; then
  [ -n "$PIDFILE" ] || { echo "wait_pid: need a pidfile or --pid" >&2; exit 3; }
  [ -r "$PIDFILE" ] || { echo "wait_pid: cannot read pidfile: $PIDFILE" >&2; exit 3; }
  PID="$(tr -cd '0-9' < "$PIDFILE")"
fi
[ -n "$PID" ] || { echo "wait_pid: pidfile held no pid: $PIDFILE" >&2; exit 3; }

# kill -0 asks "does this pid exist and may I signal it" — an identity test, not
# a text match. It cannot match this script.
if ! kill -0 "$PID" 2>/dev/null; then
  echo "wait_pid: pid $PID already gone"; exit 0
fi

echo "wait_pid: waiting on pid $PID (poll ${POLL}s${TIMEOUT:+, timeout ${TIMEOUT}s})"
WAITED=0
while kill -0 "$PID" 2>/dev/null; do
  sleep "$POLL"
  WAITED=$((WAITED + POLL))
  if [ "$TIMEOUT" -gt 0 ] && [ "$WAITED" -ge "$TIMEOUT" ]; then
    echo "wait_pid: TIMEOUT after ${WAITED}s — pid $PID still alive" >&2
    exit 2
  fi
done
echo "wait_pid: pid $PID exited after ${WAITED}s"
