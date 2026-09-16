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

# ---- ccache-on-tmpfs guard (2026-07-29) -------------------------------------
# The ccache store may live on a separate spindle to keep its small-random IO off
# the source/build/scratch disk. If that store is configured under a mount that
# is NOT currently mounted, the path resolves to whatever filesystem is
# underneath — and on this box the natural candidates (/run/media/..., /tmp) are
# tmpfs, i.e. RAM. ccache would then happily fill memory until the machine
# thrashes. That exact shape already cost a gate here: per-shard AOT caches were
# rooted under ${TMPDIR:-/tmp} and exhausted the 16G tmpfs, killing all 8 shards
# with "Disk quota exceeded" and reporting it as an untrustworthy undercount.
#
# So: refuse to start a heavy op when ccache's configured cache_dir sits on a
# tmpfs/ramfs. Generic on purpose — it keys on the FILESYSTEM TYPE, not on any
# particular path, so it keeps protecting after the store moves again. Cheap
# (one ccache -k + one stat). Bypass with BEZEL_ALLOW_TMPFS_CCACHE=1.
if [ -z "${BEZEL_ALLOW_TMPFS_CCACHE:-}" ] && command -v ccache >/dev/null 2>&1; then
  _cc_dir=$(ccache -k cache_dir 2>/dev/null || true)
  if [ -n "${_cc_dir:-}" ]; then
    _cc_fs=$(stat -f -c %T "$_cc_dir" 2>/dev/null || echo missing)
    case "$_cc_fs" in
      tmpfs|ramfs)
        echo "dev_heavy: ABORT — ccache cache_dir '$_cc_dir' is on $_cc_fs (RAM)." >&2
        echo "  Its drive is probably not mounted. Mount it, or point cache_dir at real" >&2
        echo "  disk, or set BEZEL_ALLOW_TMPFS_CCACHE=1 if you really mean it." >&2
        exit 2 ;;
      missing)
        echo "dev_heavy: WARNING — ccache cache_dir '$_cc_dir' does not exist yet." >&2 ;;
    esac
  fi
fi
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
    # Only the umbrella "bezel_tests" target gets priority — NOT bezel_tests_<area>
    # (producer lanes run the latter in parallel during fan-out; granting them
    # gate priority dilutes the gate build's CPU weight to nothing).
    if printf '%s\n' "$@" | grep -qE '\bbezel_tests\b'; then
      cw=10000; iw=800; mem=14G
    else
      cw=100;   iw=80;  mem=12G
    fi ;;
  suite)
    # per gate audit F1: the suite cage is the knob that bounds shard
    # concurrency, NOT the slice budget the gate.sh comment cites. gate.sh caps
    # the ASan pass at 8 shards and sizes that against "the ~24-28G slice" —
    # but every suite op actually runs in THIS cage, which is 12G. Making it a
    # variable lets the ASan concurrency be raised and measured against the
    # thing that really constrains it. Default unchanged (12G).
    cw=10000; iw=800; mem="${BEZEL_SUITE_MEM:-12G}" ;;
  *)
    # probe: tiny; outside the slice, its own small cage, no accounting overhead.
    exec systemd-run --user --scope -q -p MemoryMax=4G -p MemorySwapMax=0 -- "$@" ;;
esac

exec systemd-run --user --scope -q \
  -p Slice="$SLICE" -p MemoryMax="$mem" -p MemorySwapMax=0 \
  -p CPUWeight="$cw" -p IOWeight="$iw" \
  -- "$@"
