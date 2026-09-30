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
# dir): 3 failed starts in 2 min leave the unit failed; `start` resets it.
#
# `start` embeds $js (below) in the transient unit's ExecStart, and a later
# Restart=on-failure restart re-execs that SAME path. A linked worktree (any
# `git worktree add`, including a lane) can be torn down while the daemon it
# started keeps running — python already read the whole file into memory once
# `serve` starts, so the daemon PROCESS survives (see build_jobserver.py's
# module docstring) — but the NEXT automatic restart would then try to exec a
# path that no longer exists, and the daemon would silently stop coming back.
# A first attempt at fixing this tried "resolve $js to a stable path instead"
# (git's common dir, shared by every worktree, i.e. the main checkout) —
# that turned out NOT to be simpler or more robust: this repo routinely runs
# several worktrees on independent branches/commits at once (see CLAUDE.md,
# "Dual-coordinator runs"), so the main checkout's OWN working tree can be
# sitting on a branch that predates this very script, or holds a stale copy
# of it (measured while building this fix: the main checkout here had no
# scripts/build_jobserver.py at all). So instead: `start` REFUSES from a
# linked worktree, with a clear message and nonzero exit; `stop`/`status`
# are unaffected (they never touch ExecStart). A worktree meant to be the
# daemon's dedicated, long-lived home (not a lane that will be reaped) opts
# in with BEZEL_JOBSERVER_ALLOW_WORKTREE=1.
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

# #4990: the systemd unit name follows the token dir instead of being fixed.
# With a fixed name, any real `start` against another --dir (a test, a lane,
# a second pool) targeted the production daemon's unit; systemd refused the
# duplicate, but the collision was structural. Rules:
#   BEZEL_JOBSERVER_UNIT set      -> that name, verbatim
#   token dir is the default      -> bezel-jobserver  (unchanged, so status/stop
#                                    still find the instance already running)
#   any other token dir           -> bezel-jobserver-<sha256(dir)[:8]>
# Several pools can then run at once, one unit per dir, and a running default
# instance needs no migration. This reads BEZEL_JOBSERVER_DIR with the same
# default as `dir` below; trailing slashes are ignored so ".../jobserver/"
# still maps to the default unit.
_default_dir=/var/tmp/bezel-scratch/jobserver
_unit_dir="${BEZEL_JOBSERVER_DIR:-$_default_dir}"
while [ "${#_unit_dir}" -gt 1 ] && [ "${_unit_dir%/}" != "$_unit_dir" ]; do
  _unit_dir="${_unit_dir%/}"
done
if [ -n "${BEZEL_JOBSERVER_UNIT:-}" ]; then
  UNIT="$BEZEL_JOBSERVER_UNIT"
elif [ "$_unit_dir" = "$_default_dir" ]; then
  UNIT=bezel-jobserver
else
  UNIT="bezel-jobserver-$(printf '%s' "$_unit_dir" | sha256sum | cut -c1-8)"
fi
if ! [[ "$UNIT" =~ ^[A-Za-z0-9:_.@-]+$ ]]; then
  echo "jobserver_ctl: invalid systemd unit name '$UNIT' (BEZEL_JOBSERVER_UNIT)" >&2
  exit 64
fi
here="$(cd "$(dirname "$0")" && pwd)"
js="$here/build_jobserver.py"
dir="${BEZEL_JOBSERVER_DIR:-/var/tmp/bezel-scratch/jobserver}"

# See the header comment: refuse `start` from a linked worktree unless the
# operator opts in explicitly. A linked worktree's --git-dir lives under the
# common dir's worktrees/ subdirectory; the main checkout's --git-dir IS the
# common dir. `--path-format=absolute` (git >= 2.31) sidesteps the relative-
# vs-absolute spelling difference between the two ("./.git" vs
# "/path/.git/worktrees/name") so the comparison cannot be fooled by that.
if [ "${1:-status}" = start ] && [ -z "${BEZEL_JOBSERVER_ALLOW_WORKTREE:-}" ]; then
  _gd="$(git -C "$here" rev-parse --path-format=absolute --git-dir 2>/dev/null || true)"
  _gcd="$(git -C "$here" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
  if [ -n "$_gd" ] && [ -n "$_gcd" ] && [ "$_gd" != "$_gcd" ]; then
    echo "jobserver_ctl: refusing to start from a LINKED worktree ($here)." >&2
    echo "  A linked worktree (this includes a lane) can be torn down while the" >&2
    echo "  daemon runs, breaking its Restart=on-failure path silently later." >&2
    echo "  Start it from the main checkout, or set" >&2
    echo "  BEZEL_JOBSERVER_ALLOW_WORKTREE=1 if this worktree is a dedicated," >&2
    echo "  long-lived home for the daemon (not a lane that will be reaped)." >&2
    exit 1
  fi
fi

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
      echo "last audits (epoch available held makes clang mem_avail_mb repaired launchers):"
      tail -n 5 "$dir/samples.tsv"
    fi
    ;;
  *)
    echo "usage: $0 {start [TOKENS]|stop|status}" >&2
    exit 64
    ;;
esac
