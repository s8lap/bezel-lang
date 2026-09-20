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
# Tightened 2026-09-17 after OOM-kill at 23G peak (system 31G; KDE+Claude ~6G
# base; overcommit caused kernel OOM). New envelope leaves ~9G headroom for
# system + interactive + Claude, while still fitting 1 gate suite + 1 gate
# build under the cap.
# ManagedOOMMemoryPressure=auto opts this slice out of Fedora's per-slice default
# (/usr/lib/systemd/user/slice.d/10-oomd-per-slice-defaults.conf sets kill@80% on
# EVERY user slice). Without it systemd-oomd can kill the whole slice mid-gate and
# the run reads it as a shard crash. MemoryMax below is the real bound: a runaway
# is contained by the cgroup limit, which is more precise than an oomd slice kill.
systemctl --user set-property "$SLICE" \
  CPUQuota=1800% CPUWeight=60 MemoryHigh=18G MemoryMax=22G IOWeight=50 \
  ManagedOOMMemoryPressure=auto \
  >/dev/null 2>&1 || true

case "$kind" in
  build)
    # Gate build (bezel_tests) is heavy + on the critical path -> priority.
    # Producer build (bezelc etc.) is light -> background weight, uses spare cap.
    # Cages sized so 1 gate-build (12G) + 1 gate-suite (10G) fits the 22G slice
    # with ~0G slack — gate-needs-zero-builders means producers must STOP while
    # the gate runs. Producer cage (8G) fits bezelc compile peaks (~3-4G) with
    # headroom for ccache + page cache.
    if printf '%s\n' "$@" | grep -qE '\bbezel_tests\b'; then
      cw=10000; iw=800; mem=12G
    else
      cw=100;   iw=80;  mem=8G
    fi ;;
  suite)
    # Tightened from 12G default to 10G to fit the new 22G slice envelope with
    # the gate-build cage (12G) under it. ASan pass already peaks ~1G ANON, so
    # 10G has ample headroom for the page-cache file mapping.
    cw=10000; iw=800; mem="${BEZEL_SUITE_MEM:-10G}" ;;
  *)
    # probe: tiny; its own small cage, separate accounting from the heavy ops.
    # It must still NOT default into app.slice: that is where the interactive
    # terminal and the Claude session live, and probe pressure there gets THEM
    # killed by systemd-oomd rather than the probe (three konsole tabs died this
    # way on 2026-09-20, one taking a running gate with it). Give probes their own
    # slice, likewise opted out of the kill-on-pressure default.
    systemctl --user set-property bezel-probe.slice \
      MemoryMax=6G ManagedOOMMemoryPressure=auto >/dev/null 2>&1 || true
    exec systemd-run --user --scope -q -p Slice=bezel-probe.slice \
      -p MemoryMax=4G -p MemorySwapMax=0 -- "$@" ;;
esac

exec systemd-run --user --scope -q \
  -p Slice="$SLICE" -p MemoryMax="$mem" -p MemorySwapMax=0 \
  -p CPUWeight="$cw" -p IOWeight="$iw" \
  -- "$@"
