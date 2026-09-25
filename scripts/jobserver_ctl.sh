#!/usr/bin/env bash
# jobserver_ctl.sh — start / stop / inspect the machine-wide build jobserver
# (scripts/build_jobserver.py) as a systemd --user service.
#
#   scripts/jobserver_ctl.sh start [TOKENS]   default $BEZEL_JOBSERVER_TOKENS or 20
#   scripts/jobserver_ctl.sh stop
#   scripts/jobserver_ctl.sh status           daemon line + last audit samples
#
# Why a systemd service and not `nohup … &`: a daemon started from a terminal
# lives in that terminal's cgroup, and systemd-oomd kills interactive terminal
# scopes under memory pressure (see protocol-gate, 2026-09-21). `setsid` does
# not change the cgroup. A transient --user unit has its own cgroup and
# Restart=on-failure brings it back if it dies.
#
# The start limit stops a restart loop (e.g. another daemon already serves the
# dir, or the worktree holding this script was removed): 3 failed starts in
# 2 min leave the unit failed; `start` resets it. Start it from the main
# checkout, not a lane worktree, so a later restart still finds the script.
#
# Why 20 tokens: the heavy slice has ~18 cores, and every joined build keeps
# one implicit job on top of the shared tokens, so 20 tokens with a handful of
# concurrent builds runs ~22–26 compiles: enough to keep the cores busy, far
# below the 100+ that throttled the box in R20. Lower it if MemAvailable
# still dips under ~6G; raising it buys little (the cores are the ceiling).
#
# With no daemon running, dev_heavy.sh builds exactly as before (private -jN);
# gate.sh always opts out (BEZEL_JOBSERVER=off). Stopping the daemon while
# builds run is safe: makes already joined keep their open FIFO and finish;
# new builds fall back to a private pool.
set -euo pipefail

UNIT=bezel-jobserver
here="$(cd "$(dirname "$0")" && pwd)"
js="$here/build_jobserver.py"
dir="${BEZEL_JOBSERVER_DIR:-/var/tmp/bezel-scratch/jobserver}"

case "${1:-status}" in
  start)
    tokens="${2:-${BEZEL_JOBSERVER_TOKENS:-20}}"
    if python3 "$js" alive --dir "$dir"; then
      echo "jobserver_ctl: already running"; python3 "$js" status --dir "$dir"; exit 0
    fi
    # A failed earlier unit of the same name blocks a new transient one.
    systemctl --user reset-failed "$UNIT" 2>/dev/null || true
    # python reads the whole script at start, so a later worktree removal
    # cannot break the running daemon.
    systemd-run --user --unit="$UNIT" -q \
      -p Restart=on-failure -p RestartSec=5 \
      -p StartLimitIntervalSec=120 -p StartLimitBurst=3 \
      -E BEZEL_JOBSERVER_DIR="$dir" \
      python3 "$js" serve --dir "$dir" --tokens "$tokens"
    for _ in $(seq 1 50); do
      python3 "$js" alive --dir "$dir" && break
      sleep 0.1
    done
    python3 "$js" status --dir "$dir"
    ;;
  stop)
    systemctl --user stop "$UNIT" 2>/dev/null || true
    systemctl --user reset-failed "$UNIT" 2>/dev/null || true
    if python3 "$js" alive --dir "$dir"; then
      echo "jobserver_ctl: a daemon not started by this unit still serves $dir" >&2
      python3 "$js" status --dir "$dir" >&2
      exit 1
    fi
    echo "jobserver_ctl: stopped"
    ;;
  status)
    python3 "$js" status --dir "$dir" || exit 1
    if [ -f "$dir/samples.tsv" ]; then
      echo "last audits (epoch available held makes clang mem_avail_mb repaired):"
      tail -n 5 "$dir/samples.tsv"
    fi
    ;;
  *)
    echo "usage: $0 {start [TOKENS]|stop|status}" >&2
    exit 64
    ;;
esac
