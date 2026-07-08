#!/usr/bin/env bash
# Run heavy dev ops (builds, suite runs) inside a shared RESOURCE-CAPPED slice so
# many run CONCURRENTLY without OOM or CPU oversubscription — replacing the
# one-heavy-op-at-a-time lock, which starved/stalled producer builds behind
# gates. The slice caps TOTAL cpu+mem across ALL heavy ops (18 of 20 cores; 24G
# soft / 28G hard), so concurrency is self-limiting: N concurrent builds just
# share the budget (each slower) instead of oversubscribing or OOMing. Gate ops
# (the heavy bezel_tests build + suite runs) get high CPU/IO weight → they get
# most of the budget and stay fast; producer bezelc builds run at low weight,
# using spare capacity in the background. 2 cores stay outside the slice for the
# interactive session. NB do NOT reintroduce an exclusive flock — that is what
# caused the producer-stall storm; the slice bounds resources without serializing.
#
# systemd-run --scope BLOCKS the caller for the command's duration, so an agent's
# `dev_heavy build` waits inline for its build (no background-and-stall).
#
# Scratch artifacts -> /var/tmp/bezel-scratch, never /tmp (tmpfs=RAM).
set -u
if [ "$#" -lt 1 ]; then
  echo "usage: dev_heavy.sh {build|suite|probe} cmd..." >&2
  exit 2
fi
kind="$1"; shift

SLICE=bezel-heavy.slice
# Idempotently set the total budget for all heavy ops. Cheap; safe to repeat.
systemctl --user set-property "$SLICE" \
  CPUQuota=1800% CPUWeight=60 MemoryHigh=24G MemoryMax=28G IOWeight=50 \
  >/dev/null 2>&1 || true

case "$kind" in
  build)
    # Gate build (bezel_tests) is heavy + on the critical path -> priority.
    # Producer build (bezelc etc.) is light -> background weight, uses spare cap.
    if printf '%s\n' "$@" | grep -q 'bezel_tests'; then
      cw=10000; iw=800; mem=14G
    else
      cw=100;   iw=80;  mem=12G
    fi ;;
  suite)
    cw=10000; iw=800; mem=12G ;;
  *)
    # probe: tiny; outside the slice, its own small cage, no accounting overhead.
    exec systemd-run --user --scope -q -p MemoryMax=4G -p MemorySwapMax=0 -- "$@" ;;
esac

exec systemd-run --user --scope -q \
  -p Slice="$SLICE" -p MemoryMax="$mem" -p MemorySwapMax=0 \
  -p CPUWeight="$cw" -p IOWeight="$iw" \
  -- "$@"
