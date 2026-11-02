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
# Usage:  scripts/run_status.sh [--staging <worktree-path>] [--target N (default 100)]
#                               [--pool N (default 7)] [--claims <path>]
# --target is the OWNER's "gate 100 fixes at a time" directive (R35), not a
# coordinator-chosen number: the staging row FLAGS and prints an ACTION line
# once that many fix issues are banked. Raise it with --target for a run whose
# directive says otherwise.
# --pool is the run's firm pool size (the standing rule is 7 agents); --claims
# is the run's claims.tsv, whose live claims are counted to report the pool.
# Exit 0 always, unless the script itself errors. Every probe is tolerant: a
# missing tool or file reads `n/a`, never a crash.
#
# Rows:
#   gate       pidfile alive (kill -0), donefile, last line of newest *_cycle.log.
#              FLAG: pidfile present but its pid is dead (stale; see CLAUDE.md).
#   jobserver  systemctl --user is-active bezel-jobserver (+ jobserver_ctl status).
#              INFO (not FLAG) when inactive: the jobserver is opt-in.
#   builders   scripts/no_builders.sh.  FLAG: a gate is running AND builders exist.
#              The row also carries a 5-second window: a single-instant sample
#              of a box that alternates between building and analysing reads
#              "CLEAN" while every lane is mid-run.
#   memory     MemAvailable.  FLAG: < 6G AND vmstat `so` > 0 in both of the last
#              two `vmstat 1 3` samples (sustained swap-out, not a blip).
#   disk       /mnt/fast use%; aot-bincache and aot-harness must be symlinks.
#              FLAG: >= 90%, or either is not a symlink.
#   staging    (with --staging) branch, tip, commits over origin/main, and distinct
#              fix issues banked (scripts/pick_ledger.sh: kind=fix commits
#              only, see scripts/pick_issues.sh). FLAG: banked >= --target.
#   lanes      active lane count from scripts/lane_status.sh, with the live
#              per-lane build-process count and the box-wide heavy-process count.
#   pool       live lanes of the firm pool, counted from the run's claims.tsv
#              (a lane is live while it holds a `live` claim). FLAG: a PARTIAL
#              pool (0 < live < --pool) — the rule is "refilled in the same
#              turn a slot frees". INFO: 0 lanes (a drained pool is gate-ready),
#              or no claims file to count.
#   stale      dry-run count of stale dev processes/waiters (scripts/reap_stale.sh).
#              INFO, never FLAG and never an ACTION line: a non-zero count is
#              NORMAL between sweeps and just means "reap_stale.sh --kill".
#
# Env overrides (for tests and other machines):
#   BEZEL_GATE_PIDFILE, BEZEL_GATE_DONEFILE   gate markers (as gate_and_push.sh)
#   BEZEL_SCRATCH          scratch root (default /var/tmp/bezel-scratch)
#   BEZEL_AOT_FAST_ROOT    fast-disk mount (default /mnt/fast, as pregate.sh)
#   BEZEL_MEMINFO          meminfo file (default /proc/meminfo)
#   BEZEL_STATUS_CLAIMS    claims.tsv path (default $BEZEL_SCRATCH/claims.tsv)
#   BEZEL_STATUS_SKIP      space-separated row names to omit (e.g. "lanes jobserver")
set -uo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SCRATCH="${BEZEL_SCRATCH:-/var/tmp/bezel-scratch}"
PIDFILE="${BEZEL_GATE_PIDFILE:-$SCRATCH/gate.pid}"
DONEFILE="${BEZEL_GATE_DONEFILE:-$SCRATCH/gate.done}"
FAST="${BEZEL_AOT_FAST_ROOT-/mnt/fast}"
MEMINFO="${BEZEL_MEMINFO:-/proc/meminfo}"
SKIP=" ${BEZEL_STATUS_SKIP:-} "
STAGING="" TARGET=100
POOL=7
CLAIMS="${BEZEL_STATUS_CLAIMS:-$SCRATCH/claims.tsv}"
while [ $# -gt 0 ]; do
  case "$1" in
    --staging|--target|--pool|--claims)
      if [ $# -lt 2 ] || [ -z "$2" ]; then
        echo "run_status: $1 needs a value" >&2; exit 64
      fi
      case "$1" in
        --staging) STAGING="$2" ;;
        --target)  TARGET="$2" ;;
        --pool)    POOL="$2" ;;
        --claims)  CLAIMS="$2" ;;
      esac
      shift 2 ;;
    -h|--help) sed -n '13,16p' "$0"; exit 0 ;;
    *) echo "run_status: unknown argument '$1'" >&2; exit 64 ;;
  esac
done

case "$TARGET" in ''|*[!0-9]*) echo "run_status: --target must be a non-negative integer, got '$TARGET'" >&2; exit 64 ;; esac
case "$POOL"   in ''|*[!0-9]*) echo "run_status: --pool must be a non-negative integer, got '$POOL'" >&2; exit 64 ;; esac

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
  # Opt-in (CLAUDE.md): off is a legitimate state, so INFO without an ACTION.
  [ "$act" = active ] || st=INFO
  row jobserver "$st" "$act${line:+ | $line}"
}

# A single-instant sample of a box that ALTERNATES between building and
# analysing reads "CLEAN" even while four lanes are mid-run. MEASURED
# 2026-11-02 23:33:13: the heartbeat saw ZERO build processes while three
# lanes were between builds and a fourth was linking; five seconds later
# three were building, and eight seconds after that none were. The GATE
# PRECONDITION stays instantaneous -- no_builders.sh is unchanged, and "is
# anything building NOW?" is exactly the question a gate must ask, since a
# builder that starts a second later is just as disqualifying. But the
# heartbeat also answers "is the box working?", and that needs a WINDOW. So
# sample five times at one second and report how many samples saw a builder.
window_builders() {
  local i seen=0 max=0 n
  for i in 1 2 3 4 5; do
    n="$("$HERE/no_builders.sh" 2>/dev/null |
         awk '/^  [a-z]+ +[0-9]+$/ {t += $2} END {print t + 0}')"
    [ -n "$n" ] || n=0
    [ "$n" -gt "$max" ] && max=$n
    [ "$n" -gt 0 ] && seen=$((seen + 1))
    [ "$i" -lt 5 ] && sleep 1
  done
  printf '5s window: %s/5 samples saw a builder (max %s proc(s))' "$seen" "$max"
}

probe_builders() {
  local st=OK out win
  if [ -x "$HERE/no_builders.sh" ]; then out="$("$HERE/no_builders.sh" -q 2>/dev/null | tail -n 1)"
  else out="n/a"; fi
  case "$out" in *CLEAN*|n/a) ;; *)
    if [ "$GATE_RUNNING" -eq 1 ]; then st=FLAG
      ACTIONS+=("builders: a gate is running WITH builders — its verdict is untrustworthy; stop the builders")
    fi ;;
  esac
  if [ -x "$HERE/no_builders.sh" ]; then
    win="$(window_builders)"
    row builders "$st" "$out | $win"
  else
    row builders "$st" "$out"
  fi
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
              | sed -n 's/^distinct fix issues: \([0-9]*\).*/\1/p')"
  [ -n "$banked" ] || banked="n/a"
  if [ "$banked" != n/a ] && [ "$banked" -ge "$TARGET" ]; then st=FLAG
    ACTIONS+=("staging: $banked fixes banked (target $TARGET) — run scripts/pregate.sh, then gate")
  fi
  row staging "$st" "${br:-(detached)} @ $tip | +$ahead over origin/main | banked $banked/$TARGET"
}

# The LIVE POOL. A claim's CURRENT status is its LAST row (claims.tsv is
# append-only), and a lane is LIVE while it holds at least one claim whose
# status is `live` (the role may follow a colon: `live:producing`,
# `live:reviewing`). The count is of DISTINCT LANES holding a live claim, not
# of claims: one lane fixing three issues is one lane. A lane that also holds
# a QUEUED claim for a later issue is still live, which is why the live rows
# are counted and the queued ones ignored rather than one status per lane.
#
# "LAST ROW WINS" IS PER CLAIM, NOT PER LANE — a lane that finishes one issue
# and still has another open is live, and a lane whose only claim was
# superseded by a later `done` row is not. Keying by (lane, issue) and then
# counting the lanes is what makes both true.
#
# WHY THIS ROW EXISTS. The pool is a firm size, refilled in the same turn a
# slot frees, and until 2026-11-02 nothing could SEE it: a run left a slot
# empty for nineteen minutes and no row said so. A drained pool is legitimate
# (it is what a gate wants), so 0 lanes is INFO, not a flag; a PARTIAL pool is
# a flag, because the rule is "refilled in the same turn a slot frees".
probe_pool() {
  local st=OK lanes="" detail
  if [ ! -f "$CLAIMS" ]; then
    row pool INFO "n/a (no claims file at $CLAIMS — pass --claims <path>)"
    return 0
  fi
  lanes="$(awk -F'\t' '
      $1 != "issue" { st[$2 SUBSEP $1] = $NF }
      END { for (k in st) { split(k, a, SUBSEP)
             if (st[k] ~ /^live/ && !(a[1] in seen)) { seen[a[1]] = 1; n++ } }
           print n + 0 }' "$CLAIMS")"
  [ -n "$lanes" ] || lanes=0
  detail="$lanes live lane(s) of $POOL"
  if [ "$GATE_RUNNING" -eq 1 ]; then
    row pool OK "$detail — a gate is running, and the pool is zero by rule"
    return 0
  fi
  if [ "$lanes" -eq 0 ]; then
    row pool INFO "$detail — the pool is drained (gate-ready?)"
  elif [ "$lanes" -lt "$POOL" ]; then
    st=FLAG
    ACTIONS+=("pool: $lanes live lane(s) of a $POOL-lane pool — refill a slot in the SAME turn it frees")
    row pool "$st" "$detail"
  else
    row pool "$st" "$detail"
  fi
}

probe_lanes() {
  local out n procs heavy
  out="$("$HERE/lane_status.sh" 2>/dev/null)" || true
  if printf '%s' "$out" | grep -q 'no lane worktrees active'; then n=0
  else n="$(printf '%s\n' "$out" | sed -n '2,/^$/p' | grep -c .)"; fi
  local stall; stall="$(printf '%s\n' "$out" | grep -c 'STALL?')"
  # The table's 6th column is the live build/test process count rooted in each
  # lane's tree, and the footer carries the whole-box heavy-process count.
  # Without these the lanes row says "4 active lane worktree(s)" and nothing
  # about whether any of them is BUILDING -- which is the question a heartbeat
  # is really asked. (The "-- box:" footer is excluded by its leading dashes.)
  procs="$(printf '%s\n' "$out" |
           awk '!/^--/ && NF >= 6 && $6 ~ /^[0-9]+$/ {s += $6} END {print s + 0}')"
  heavy="$(printf '%s\n' "$out" | sed -n 's/.*heavy procs: \([0-9]*\).*/\1/p' | head -n 1)"
  [ -n "$procs" ] || procs=0
  [ -n "$heavy" ] || heavy=0
  row lanes OK "$n active lane worktree(s), $procs lane build proc(s) live, $heavy heavy proc(s) box-wide, $stall stall candidate(s) (scripts/lane_status.sh for detail)"
}

# Stale dev processes and waiters. R27 2026-10-06: a lane's agent can die while
# its background waiters keep running, parented to this session's long-lived
# `claude` rather than to init — which is why reap_orphans.sh (PPID==1 only)
# never saw a 4.25 h waiter. reap_stale.sh keys on age AND parent plus a
# CPU/IO progress exemption, so a slow build is never mistaken for a wedged one.
#
# INFO, never FLAG, and it never signals anything: this is a dry-run count.
# A non-zero count is NORMAL between sweeps — it means "run reap_stale.sh --
# kill", not "something is wrong". A count that GROWS across two consecutive
# heartbeats is the signal worth acting on, which is why it is a count and not
# a boolean.
probe_stale() {
  local out n
  [ -x "$HERE/reap_stale.sh" ] || { row stale n/a "reap_stale.sh absent"; return 0; }
  out="$("$HERE/reap_stale.sh" 2>/dev/null)" || true
  n="$(printf '%s\n' "$out" | sed -n 's/^candidates: \([0-9]*\).*/\1/p' | head -n 1)"
  [ -n "$n" ] || n=0
  local st="INFO"
  # A live gate makes every long process legitimate; the row is noise then.
  [ "$GATE_RUNNING" = 1 ] && { row stale INFO "gate running — reaping suppressed"; return 0; }
  # INFO means INFO: the row REPORTS a non-zero count and deliberately does not
  # push an ACTION. A stale-process count is expected between sweeps — it means
  # "run reap_stale.sh --kill", not "something is wrong" — so escalating it to
  # an ACTION would put a line in every heartbeat's output on any machine that
  # happens to have a long build running. scripts/gate/tests/test_run_status.py's
  # `test_quiet_sandbox_prints_ok_gate_row_and_no_actions` pins exactly that: a
  # quiet sandbox prints no `ACTION:` line, and this row was the one that broke
  # it. What IS worth acting on is a count that GROWS across two consecutive
  # heartbeats, which is why this is a count and not a boolean.
  row stale "$st" "$n stale dev process(es) (scripts/reap_stale.sh --kill to reap; dry-run is the default, PID-only, never while a gate is live)"
}

echo "run_status $(date '+%F %T')"
want gate && probe_gate
want jobserver && probe_jobserver
want builders && probe_builders
want memory && probe_memory
want disk && probe_disk
[ -n "$STAGING" ] && want staging && probe_staging
want lanes && probe_lanes
want pool && probe_pool
want stale && probe_stale
for a in "${ACTIONS[@]}"; do echo "ACTION: $a"; done
exit 0
