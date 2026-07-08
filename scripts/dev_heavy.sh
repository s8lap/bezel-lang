#!/usr/bin/env bash
# Run a heavy dev command (full build / full test suite) under the GLOBAL
# heavy-op lock, in a right-sized memory cage, at background CPU/IO weight.
#
# Why: development agents run in parallel worktrees; unbounded concurrent
# -j16 builds + 16-shard suite runs oversubscribe the 20-core / 32 GiB box
# (CPU 2-3x, RAM past physical at peak) and page-cache thrash makes the
# whole machine crawl. One heavy op at a time is cheaper in wall-clock than
# two thrashing ones, and background weights keep the interactive session
# responsive while it runs.
#
# Usage:
#   scripts/dev_heavy.sh build <cmd...>   # e.g. cmake --build build -j16
#   scripts/dev_heavy.sh suite <cmd...>   # e.g. tests/run_parallel.sh 16
#   scripts/dev_heavy.sh probe <cmd...>   # small compile/run: cage only, no lock
#
# Scratch artifacts (probe binaries, big logs, scratch build trees) belong in
# /var/tmp/bezel-scratch (DISK-backed), never /tmp — Fedora's /tmp is tmpfs
# (RAM): a scratch build tree parked there permanently eats gigabytes of RAM.
set -u
# NB: do NOT fold the usage string into `${1:?...}` — a literal `}` in the message
# (e.g. `{build|suite|probe}`) prematurely closes the `${...}` expansion, so `kind`
# silently captured `$1` plus trailing message text ("build cmd...}") and NEVER
# matched the case below → every call fell through to the `*)` default (lock=0,
# mem=4G). That single quoting bug is why builds never serialized (OOM vector) and
# why caged test-binary runs were killed at 4G. Keep the guard as a plain check.
if [ "$#" -lt 1 ]; then
  echo "usage: dev_heavy.sh {build|suite|probe} cmd..." >&2
  exit 2
fi
kind="$1"; shift
case "$kind" in
  build) mem=12G; lock=1 ;;
  suite) mem=8G;  lock=1 ;;
  *)     mem=4G;  lock=0 ;;
esac
run=(systemd-run --user --scope -q
     -p "MemoryMax=$mem" -p MemorySwapMax=0
     -p CPUWeight=40 -p IOWeight=40
     -- "$@")
# Serialize heavy ops across all worktrees/agents (32 GiB box; concurrent -j16
# builds OOM). Hold the lock on a dedicated FD (9) owned by THIS script process,
# then run the command. The FD stays locked until the script exits, so the lock
# spans the whole build/suite.
#
# Why not `flock <file> <cmd>`? Empirically (2026-07-08) the `flock`-wraps-command
# form releases the lock mid-build here: `flock` exec-chains into
# `systemd-run --scope`, which registers the scope and hands the command off, and
# the lock holder goes away ~1s in — so two `dev_heavy build` ran CONCURRENTLY
# (the real OOM vector this whole session). `systemd-run --scope` DOES block the
# caller for the command's duration, so holding the lock on a script-owned FD and
# then blocking on `--scope` keeps it held the entire time. (Service mode
# `--wait --pipe` would also block but runs in $HOME and drops env vars like
# BEZEL_SHARD_*/TMPDIR/ASAN_OPTIONS — unusable for the suite; `--scope` keeps CWD
# and the full environment.) Verified: two `dev_heavy build sleep 5` take ~10s
# (serial) with this form vs ~5s (concurrent) with `flock <file> <cmd>`.
# RE-ENTRANCY: the lock must be re-entrant across nested dev_heavy calls, or a
# `dev_heavy suite <x>` whose <x> itself calls `dev_heavy build` (e.g.
# asan_sweep.sh builds its ASan tree via dev_heavy) DEADLOCKS: the outer holds
# the lock and waits for <x>; the inner blocks forever on `flock -x 9`. We mark
# the environment when we take the lock; a nested call sees the mark and runs
# without re-acquiring (still caged by its own systemd-run scope). systemd-run
# --scope inherits the caller's env, so the mark propagates to grandchildren.
if [ "$lock" = 1 ] && [ -z "${BEZEL_HEAVY_LOCK_HELD:-}" ]; then
  exec 9>/var/tmp/bezel-heavy.lock
  flock -x 9
  export BEZEL_HEAVY_LOCK_HELD=1
  "${run[@]}"
else
  "${run[@]}"
fi
