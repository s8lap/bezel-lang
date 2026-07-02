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
kind="${1:?usage: dev_heavy.sh {build|suite|probe} cmd...}"; shift
case "$kind" in
  build) mem=12G; lock=1 ;;
  suite) mem=8G;  lock=1 ;;
  *)     mem=4G;  lock=0 ;;
esac
run=(systemd-run --user --scope -q
     -p "MemoryMax=$mem" -p MemorySwapMax=0
     -p CPUWeight=40 -p IOWeight=40
     -- "$@")
if [ "$lock" = 1 ]; then
  exec flock /var/tmp/bezel-heavy.lock "${run[@]}"
else
  exec "${run[@]}"
fi
