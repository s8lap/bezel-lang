#!/usr/bin/env bash
# run_status.sh — the coordinator heartbeat as ONE table.
#
# WHY. CLAUDE.md makes a coordinator run a coarse heartbeat that "only prompts a
# status check". In R21 that check was eight hand-typed probes per wake-up
# (pidfile + kill -0, donefile, cycle-log tail, jobserver unit, no_builders,
# MemAvailable + vmstat, df /mnt/fast, the aot symlinks, the staging count), and
# the ones skipped were the ones that mattered: a stale pidfile after an oomd
# kill, a bincache that had become a real directory, a batch already past its
# target sitting un-gated. This prints every one of them as a row, and an
# ACTION line only for the rows that need one. It never acts itself.
#
# Usage:  scripts/run_status.sh [--staging <worktree-path>] [--target N (default 60)]
# Exit 0 always, unless the script itself errors. Every probe is tolerant: a
# missing tool or file reads `n/a`, never a crash.
#
# Rows:
#   gate       pidfile alive (kill -0), donefile, last line of newest *_cycle.log.
#              FLAG: pidfile present but its pid is dead (stale; see CLAUDE.md).
#   jobserver  systemctl --user is-active bezel-jobserver (+ jobserver_ctl status).
#              FLAG: not active.
#   builders   scripts/no_builders.sh.  FLAG: a gate is running AND builders exist.
#   memory     MemAvailable.  FLAG: < 6G AND vmstat `so` > 0 in both of the last
#              two `vmstat 1 3` samples (sustained swap-out, not a blip).
#   disk       /mnt/fast use%; aot-bincache and aot-harness must be symlinks.
#              FLAG: >= 90%, or either is not a symlink.
#   staging    (with --staging) branch, tip, commits over origin/main, and distinct
#              issues banked (scripts/pick_ledger.sh; subject-only, see
#              scripts/pick_issues.sh). FLAG: banked >= --target.
#   lanes      active lane count from scripts/lane_status.sh.
#
# Env overrides (for tests and other machines):
#   BEZEL_GATE_PIDFILE, BEZEL_GATE_DONEFILE   gate markers (as gate_and_push.sh)
#   BEZEL_SCRATCH          scratch root (default /var/tmp/bezel-scratch)
#   BEZEL_AOT_FAST_ROOT    fast-disk mount (default /mnt/fast, as pregate.sh)
#   BEZEL_MEMINFO          meminfo file (default /proc/meminfo)
#   BEZEL_STATUS_SKIP      space-separated row names to omit (e.g. "lanes jobserver")
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SCRATCH="${BEZEL_SCRATCH:-/var/tmp/bezel-scratch}"
PIDFILE="${BEZEL_GATE_PIDFILE:-$SCRATCH/gate.pid}"
DONEFILE="${BEZEL_GATE_DONEFILE:-$SCRATCH/gate.done}"
FAST="${BEZEL_AOT_FAST_ROOT-/mnt/fast}"
MEMINFO="${BEZEL_MEMINFO:-/proc/meminfo}"
SKIP=" ${BEZEL_STATUS_SKIP:-} "
STAGING="" TARGET=60
while [ $# -gt 0 ]; do
  case "$1" in
    --staging) STAGING="${2:-}"; shift 2 ;;
    --target) TARGET="${2:-60}"; shift 2 ;;
    -h|--help) sed -n '13,15p' "$0"; exit 0 ;;
    *) echo "run_status: unknown argument '$1'" >&2; exit 64 ;;
  esac
done

ACTIONS=()
row() { printf '%-10s %-4s  %s\n' "$1" "$2" "$3"; }
want() { case "$SKIP" in *" $1 "*) return 1 ;; esac; return 0; }
have() { command -v "$1" >/dev/null 2>&1; }

GATE_RUNNING=0
probe_gate() {
  local st=OK pid="" alive="none" done_="n/a" last="n/a" log
  if [ -f "$PIDFILE" ]; then
    pid="$(tr -dc '0-9' < "$PIDFILE" 2>/dev/null)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then alive="RUNNING pid $pid"; GATE_RUNNING=1
    else alive="STALE pidfile (pid ${pid:-?} dead)"; st=FLAG
      ACTIONS+=("gate: pidfile $PIDFILE names a dead pid — confirm no gate is running, then remove it")
    fi
  fi
  [ -r "$DONEFILE" ] && done_="$(head -c 16 "$DONEFILE" | tr -d '\n')"
  log="$(ls -t "$SCRATCH"/gate-cycle/*_cycle.log 2>/dev/null | head -n 1)"
  [ -n "$log" ] && last="$(basename "$log"): $(tail -n 1 "$log" 2>/dev/null | cut -c1-110)"
  row gate "$st" "pid: $alive | done: $done_ | $last"
}

probe_jobserver() {
  local st=OK act="n/a" line=""
  have systemctl && act="$(systemctl --user is-active bezel-jobserver 2>/dev/null)"
  [ -n "$act" ] || act="unknown"
  if [ -x "$HERE/jobserver_ctl.sh" ]; then
    line="$(timeout 5 "$HERE/jobserver_ctl.sh" status 2>/dev/null | head -n 1 | cut -c1-80)"
  fi
  if [ "$act" != active ]; then st=FLAG
    ACTIONS+=("jobserver: not active ($act) — scripts/jobserver_ctl.sh start (from the main checkout)")
  fi
  row jobserver "$st" "$act${line:+ | $line}"
}

probe_builders() {
  local st=OK out
  if [ -x "$HERE/no_builders.sh" ]; then out="$("$HERE/no_builders.sh" -q 2>/dev/null | tail -n 1)"
  else out="n/a"; fi
  case "$out" in *CLEAN*|n/a) ;; *)
    if [ "$GATE_RUNNING" -eq 1 ]; then st=FLAG
      ACTIONS+=("builders: a gate is running WITH builders — its verdict is untrustworthy; stop the builders")
    fi ;;
  esac
  row builders "$st" "$out"
}

probe_memory() {
  local st=OK kb gb="n/a" so="n/a"
  kb="$(awk '/^MemAvailable:/{print $2}' "$MEMINFO" 2>/dev/null)"
  [ -n "$kb" ] && gb="$(awk -v k="$kb" 'BEGIN{printf "%.1f", k/1048576}')"
  # Columns are located by header name, so a vmstat with extra columns still parses.
  have vmstat && so="$(vmstat 1 3 2>/dev/null | awk 'NR==2{for(i=1;i<=NF;i++) if($i=="so") c=i} NR>3&&c{printf "%s ", $c}')"
  so="${so% }"; [ -n "$so" ] || so="n/a"
  if [ "$gb" != n/a ] && awk -v g="$gb" 'BEGIN{exit !(g<6)}'; then
    set -- $so
    if [ "$#" -ge 2 ] && [ "$1" != n/a ] && [ "$1" -gt 0 ] && [ "$2" -gt 0 ]; then st=FLAG
      ACTIONS+=("memory: ${gb}G available with sustained swap-out — pause producer builds (heartbeat trigger 5)")
    fi
  fi
  row memory "$st" "MemAvailable ${gb}G | vmstat so (last 2): $so"
}

probe_disk() {
  local st=OK use="n/a" links="" s
  if [ -n "$FAST" ] && [ -d "$FAST" ]; then
    use="$(df -P "$FAST" 2>/dev/null | awk 'NR==2{gsub("%","",$5); print $5}')"
    [ -n "$use" ] || use="n/a"
  fi
  if [ "$use" != n/a ] && [ "$use" -ge 90 ]; then st=FLAG
    ACTIONS+=("disk: $FAST at ${use}% — scripts/gc_scratch.sh --apply")
  fi
  for s in aot-bincache aot-harness; do
    if [ -L "$SCRATCH/$s" ]; then links+="$s->$(readlink "$SCRATCH/$s") "
    else links+="$s NOT-A-SYMLINK "; st=FLAG
      ACTIONS+=("disk: $SCRATCH/$s is not a symlink — the gate would run on slow storage; see scripts/lane_setup.sh")
    fi
  done
  row disk "$st" "${FAST:-n/a} ${use}% | ${links% }"
}

probe_staging() {
  local st=OK br tip ahead banked
  if ! git -C "$STAGING" rev-parse --git-dir >/dev/null 2>&1; then
    row staging FLAG "n/a ($STAGING is not a git worktree)"; ACTIONS+=("staging: check --staging path"); return
  fi
  br="$(git -C "$STAGING" branch --show-current 2>/dev/null)"
  tip="$(git -C "$STAGING" rev-parse --short HEAD 2>/dev/null)"
  ahead="$(git -C "$STAGING" rev-list --count origin/main..HEAD 2>/dev/null || echo n/a)"
  banked="$(cd "$STAGING" && "$HERE/pick_ledger.sh" origin/main..HEAD 2>/dev/null \
              | sed -n 's/^distinct issues: //p')"
  [ -n "$banked" ] || banked="n/a"
  if [ "$banked" != n/a ] && [ "$banked" -ge "$TARGET" ]; then st=FLAG
    ACTIONS+=("staging: $banked fixes banked (target $TARGET) — run scripts/pregate.sh, then gate")
  fi
  row staging "$st" "${br:-(detached)} @ $tip | +$ahead over origin/main | banked $banked/$TARGET"
}

probe_lanes() {
  local out n
  out="$("$HERE/lane_status.sh" 2>/dev/null)" || true
  if printf '%s' "$out" | grep -q 'no lane worktrees active'; then n=0
  else n="$(printf '%s\n' "$out" | sed -n '2,/^$/p' | grep -c .)"; fi
  local stall; stall="$(printf '%s\n' "$out" | grep -c 'STALL?')"
  row lanes OK "$n active lane worktree(s), $stall stall candidate(s) (scripts/lane_status.sh for detail)"
}

echo "run_status $(date '+%F %T')"
want gate && probe_gate
want jobserver && probe_jobserver
want builders && probe_builders
want memory && probe_memory
want disk && probe_disk
[ -n "$STAGING" ] && want staging && probe_staging
want lanes && probe_lanes
for a in "${ACTIONS[@]}"; do echo "ACTION: $a"; done
exit 0
