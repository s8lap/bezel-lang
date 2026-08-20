#!/usr/bin/env bash
# Sample the peak memory of the dev_heavy slice while a gate phase runs.
#
# per gate audit F1/Tier-0 (docs/research/2026-08-19-gate-performance-audit.md).
# The ASan pass runs at conc=8 while the plain pass runs at conc=16, and that
# halving — not instrumentation — is where its ~2.6x wall comes from (equal
# worker-seconds to within ~1.3x). Raising ASan concurrency is the single
# largest lever on gate wall, but it is bounded by the `dev_heavy.sh suite`
# memory cage, and NOBODY HAS EVER MEASURED THE ACTUAL PEAK. This measures it.
#
# Why not `systemctl show <scope> -p MemoryPeak`: the shard run's scope is a
# TRANSIENT unit that systemd releases the moment it exits, so a query after
# the phase reads a unit that no longer exists — i.e. nothing. The slice is
# likewise GC'd once its last child leaves. So the high-water mark has to be
# captured WHILE the phase is alive; hence a sampler rather than a postmortem.
#
# PURE OBSERVATION. Every failure path is silent and leaves an empty/absent
# outfile; nothing here can fail a gate, and the sampler holds no locks and
# writes nothing the gate reads for accounting.
set -u

SLICE="${BEZEL_HEAVY_SLICE:-bezel-heavy.slice}"
UID_N="$(id -u)"

# Resolve the slice's cgroup v2 directory. The canonical user-manager path
# first; a bounded `find` as the fallback so this keeps working if the systemd
# layout shifts. Empty on failure -> the caller records nothing.
cg_dir() {
  # NB the slice nests under a bezel.slice parent, so the "obvious" path
  # (user@UID.service/bezel-heavy.slice) MISSES. Probe it, then fall back to a
  # bounded find — which is what actually resolves it here.
  local c="/sys/fs/cgroup/user.slice/user-${UID_N}.slice/user@${UID_N}.service/bezel.slice/${SLICE}"
  if [ -d "$c" ]; then printf '%s\n' "$c"; return 0; fi
  find /sys/fs/cgroup -maxdepth 7 -type d -name "$SLICE" 2>/dev/null | head -1
}

case "${1:-}" in
  sample)
    # sample <outfile> [interval_s] — run until killed. Tracks the max of
    # memory.current across samples AND the kernel's own memory.peak
    # high-water mark (which cannot miss a spike between two samples). We
    # report both: peak >= current_max always, and a large gap between them
    # means the phase is spiky rather than steadily loaded.
    out="${2:?usage: mempeak.sh sample <outfile> [interval]}"
    iv="${3:-2}"
    maxcur=0; maxpeak=0; maxanon=0; n=0
    : > "$out" 2>/dev/null || exit 0
    while :; do
      d="$(cg_dir)"
      if [ -n "$d" ]; then
        cur="$(cat "$d/memory.current" 2>/dev/null || echo 0)"
        pk="$(cat "$d/memory.peak" 2>/dev/null || echo 0)"
        # THE figure: anonymous + shmem + slab. memory.current is mostly page
        # cache (measured: anon 0, file 14.3G on an idle slice), which the
        # kernel reclaims under pressure rather than OOM-killing for. Sizing
        # shard concurrency against memory.current overstates the load by ~10x.
        anon="$(awk '$1=="anon"||$1=="shmem"||$1=="slab"{t+=$2} END{print t+0}' \
                 "$d/memory.stat" 2>/dev/null || echo 0)"
        case "$cur"  in ''|*[!0-9]*) cur=0 ;; esac
        case "$pk"   in ''|*[!0-9]*) pk=0 ;; esac
        case "$anon" in ''|*[!0-9]*) anon=0 ;; esac
        [ "$cur"  -gt "$maxcur"  ] && maxcur="$cur"
        [ "$pk"   -gt "$maxpeak" ] && maxpeak="$pk"
        [ "$anon" -gt "$maxanon" ] && maxanon="$anon"
        n=$((n + 1))
        # Rewrite every sample so a killed sampler still leaves the latest
        # figures (no flush-on-exit dependency, no trap to lose).
        printf 'mem_anon_max_bytes=%s\nmem_peak_bytes=%s\nmem_current_max_bytes=%s\nmem_samples=%s\n' \
          "$maxanon" "$maxpeak" "$maxcur" "$n" > "$out" 2>/dev/null || exit 0
      fi
      sleep "$iv"
    done ;;
  start)
    # start <outfile> [interval] -> prints the sampler PID on stdout.
    #
    # NB memory.peak is NOT resettable from a shell: cgroup v2 resets it
    # per-FILE-DESCRIPTOR, so `echo 0 >` followed by `cat` (a new fd) still
    # reads the global high-water mark. Measured here, don't retry it. The
    # sampler's own max-of-anon below is the figure that matters anyway.
    out="${2:?usage: mempeak.sh start <outfile> [interval]}"
    setsid "$0" sample "$out" "${3:-2}" >/dev/null 2>&1 < /dev/null &
    printf '%s\n' "$!" ;;
  stop)
    # stop <pid> — terminate the sampler. Never fails.
    pid="${2:-}"
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
    exit 0 ;;
  report)
    # report <outfile> — human line, or nothing if there is no measurement.
    out="${2:-}"; [ -s "${out:-/nonexistent}" ] || exit 0
    # shellcheck disable=SC1090
    . "$out" 2>/dev/null || exit 0
    awk -v a="${mem_anon_max_bytes:-0}" -v p="${mem_peak_bytes:-0}" \
        -v c="${mem_current_max_bytes:-0}" -v n="${mem_samples:-0}" \
      'BEGIN { if (a+p+c == 0) exit 0
               printf "slice memory: ANON-max %.2f GiB  (current-max %.2f GiB, kernel-peak %.2f GiB incl. page cache; %d samples)\n",
                      a/1073741824, c/1073741824, p/1073741824, n }' ;;
  *)
    echo "usage: mempeak.sh {start <out> [iv] | stop <pid> | sample <out> [iv] | report <out>}" >&2
    exit 2 ;;
esac
