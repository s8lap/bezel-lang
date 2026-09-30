#!/usr/bin/env bash
# build_token.sh — per-job launcher: every compile and link that CMake runs
# takes a slot from the machine-wide build token pool before it starts, unless
# the make/ninja that started it already took one for it.
#
#   build_token.sh <command> [args...]
#
# Installed by CMakeLists.txt in front of the compiler and linker
# (CMAKE_<LANG>_COMPILER_LAUNCHER / CMAKE_<LANG>_LINKER_LAUNCHER, and the PCH
# target's own launcher in tests/CMakeLists.txt), so it wraps ccache.
#
# WHY. 2026-09-30 13:16, box-wide OOM: lane briefs said
# `cmake --build build --target bezel_tests_sema -j`, which is `make -j`, which
# is UNLIMITED. Two lanes reached 499 concurrent clang, swap filled, and the
# kernel OOM killer took three Claude sessions and plasmashell. The jobserver
# (scripts/build_jobserver.py) bounded only builds routed through dev_heavy.sh
# with argv[0] = cmake/make/ninja, and anything that lives in the command line
# can be bypassed by a different command line. The compile rule itself cannot:
# this launcher is in every generated compile/link command, whoever runs it.
#
# Decision, per job:
#   1. BEZEL_JOBSERVER=off -> run it. gate.sh sets this: a gate runs with zero
#      other builders and must not depend on a daemon.
#   2. Already accounted -> run it. The nearest make/ninja ancestor holds the
#      pool's FIFO open AND MAKEFLAGS names that FIFO, i.e. a build joined
#      through dev_heavy.sh, which took this job's token (or used its one
#      implicit slot) itself. Both checks are needed. Probed with make 4.4.1
#      and ninja 1.13.1: with an explicit -j/-jN on its command line, make
#      drops the inherited auth and never opens the FIFO. ninja does not open it
#      either, but still exports the inherited MAKEFLAGS to its jobs.
#   3. A launcher-aware daemon is live -> take one token from its FIFO
#      (blocking), run the job, put the token back. An unbounded `make -j`
#      then queues here instead of running hundreds of compilers.
#   4. No launcher-aware daemon -> take one of $BEZEL_BUILD_TOKEN_SLOTS
#      (default 16) machine-wide flock slots. Bounded even with no daemon, or
#      with an older daemon that would re-mint the tokens this script holds.
#
# The daemon counts the tokens this script holds by its process name (comm),
# which it sets itself: `bzl-tok-wait` while waiting, `bzl-tok-held` while it
# holds a token, `bzl-tok-done` while it hands it back. It keeps the FIFO open
# the whole time, so it shows up in the daemon's holder audit. A holder
# SIGKILLed with a token in hand loses it, and the daemon's audit restores it
# like a token lost by a killed make. The FIFO and slot descriptors are closed
# for the job itself, so no compiler ever holds a token.
#
# Cost: covered jobs (case 2) exec straight through after a few /proc reads
# with no fork; cases 3 and 4 add one bash process per running or queued job.
set -u

if [ "$#" -eq 0 ]; then
  echo "usage: build_token.sh <command> [args...]" >&2
  exit 64
fi

[ "${BEZEL_JOBSERVER:-on}" = off ] && exec "$@"

_dir="${BEZEL_JOBSERVER_DIR:-/var/tmp/bezel-scratch/jobserver}"
_fifo="$_dir/fifo"

_set_comm() { printf '%s' "$1" > "/proc/$BASHPID/comm" 2>/dev/null || true; }

# Case 2. Pure builtins: this runs for every job of every joined build.
_covered() {
  local mf="${MAKEFLAGS:-}" auth pid comm f key val i
  case "$mf" in *--jobserver-auth=fifo:*) ;; *) return 1 ;; esac
  auth="${mf##*--jobserver-auth=fifo:}"
  auth="${auth%% *}"
  [[ -p "$_fifo" && "$auth" -ef "$_fifo" ]] || return 1
  pid=$PPID
  # launcher <- sh <- make, or launcher <- cmake -E cmake_link_script <- sh <-
  # make; a few more levels for wrapper scripts.
  for i in 1 2 3 4 5 6 7 8; do
    [ "$pid" -gt 1 ] 2>/dev/null || return 1
    IFS= read -r comm < "/proc/$pid/comm" 2>/dev/null || return 1
    case "$comm" in
      make|gmake|ninja|ninja-build)
        # The NEAREST build tool decides: a joined outer make running an
        # unjoined inner `make -j` must not vouch for the inner jobs.
        for f in /proc/"$pid"/fd/*; do
          [[ "$f" -ef "$_fifo" ]] && return 0
        done
        return 1 ;;
    esac
    val=
    while read -r key val; do
      [ "$key" = PPid: ] && break
      val=
    done < "/proc/$pid/status" 2>/dev/null
    [ -n "$val" ] || return 1
    pid=$val
  done
  return 1
}
_covered && exec "$@"

# A daemon that counts launcher-held tokens publishes `features` naming its own
# pid. An older daemon would read our held tokens as a leak and re-mint them.
# The daemon holds its FIFO open for its lifetime, so "that pid holds this
# FIFO" is also what tells a live daemon from a SIGKILLed one whose pid was
# reused (waiting on a FIFO nobody refills would hang the build).
_daemon_ok() {
  local feat fpid pid f
  [ -p "$_fifo" ] || return 1
  { read -r feat fpid < "$_dir/features"; } 2>/dev/null || return 1
  [ "$feat" = launcher1 ] || return 1
  { read -r pid < "$_dir/pid"; } 2>/dev/null || return 1
  [ -n "$pid" ] && [ "$pid" = "$fpid" ] || return 1
  for f in /proc/"$pid"/fd/*; do
    [[ "$f" -ef "$_fifo" ]] && return 0
  done
  return 1
}

_rc=0
if _daemon_ok; then
  # Case 3. O_RDWR never blocks on open and never reads EOF.
  _fd=
  exec {_fd}<>"$_fifo" || _fd=
  [ -n "$_fd" ] && _set_comm bzl-tok-wait
  _tok=
  _poll="${BEZEL_BUILD_TOKEN_POLL:-5}"
  while [ -n "$_fd" ]; do
    if LC_ALL=C IFS= read -r -n1 -t "$_poll" -u "$_fd" _tok && [ -n "$_tok" ]; then
      break
    fi
    # Every $_poll s: if the daemon died or was replaced (a fresh FIFO), this FIFO
    # may never see another token. Give up the wait and use the slots.
    if ! _daemon_ok || ! [[ /proc/$BASHPID/fd/$_fd -ef "$_fifo" ]]; then
      exec {_fd}>&-
      _fd=
      break
    fi
  done
  if [ -n "${_fd:-}" ]; then
    _release() {
      if [ -n "${_fd:-}" ]; then
        _set_comm bzl-tok-done
        printf '%s' "$_tok" >&"$_fd" 2>/dev/null
        exec {_fd}>&-
        _fd=
      fi
    }
    trap _release EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    trap 'exit 129' HUP
    _set_comm bzl-tok-held
    "$@" {_fd}>&-
    _rc=$?
    _release
    exit "$_rc"
  fi
fi

# Case 4. flock is released by the kernel however this process dies.
_sdir="${BEZEL_BUILD_TOKEN_SLOTS_DIR:-/var/tmp/bezel-scratch/build-token-slots}"
_n="${BEZEL_BUILD_TOKEN_SLOTS:-16}"
case "$_n" in ''|*[!0-9]*|0) _n=16 ;; esac
[ -d "$_sdir" ] || mkdir -p "$_sdir" 2>/dev/null
_start=$(( RANDOM % _n ))
_lfd=
for (( _i = 0; _i < _n; _i++ )); do
  _s=$(( (_start + _i) % _n ))
  exec {_try}>>"$_sdir/slot.$_s" || continue
  if flock -n "$_try"; then
    _lfd=$_try
    break
  fi
  exec {_try}>&-
done
if [ -z "$_lfd" ]; then
  # All busy: queue on one slot. Waiters spread over every slot, so each slot
  # that frees wakes one of them.
  if exec {_lfd}>>"$_sdir/slot.$_start"; then
    flock "$_lfd" || { exec {_lfd}>&-; _lfd=; }
  fi
fi
if [ -z "$_lfd" ]; then
  # The slot dir is unusable (disk full, permissions). Fail loudly rather than
  # run unbounded, since an unbounded build is what took the box down.
  echo "build_token.sh: cannot take a build slot in $_sdir; refusing to run unbounded" >&2
  exit 75
fi
"$@" {_lfd}>&-
_rc=$?
exit "$_rc"
