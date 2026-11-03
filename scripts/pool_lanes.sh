#!/usr/bin/env bash
# pool_lanes.sh — count the LIVE LANES from OPENCODE ITSELF, not from the ledger.
#
# WHY THIS EXISTS. `run_status.sh`'s `pool` row counts the DISTINCT lanes that
# hold a `live` row in the claims ledger. A ledger row is a CLAIM, not a
# measurement: on 2026-11-03 a lane was claimed, its worktree was created, and
# NO AGENT WAS EVER DISPATCHED — the ledger said "live", the UI said "0 live
# agents", and the two disagreed. The disagreement was the LEDGER's error, not
# the UI's: the claim was filed for an agent that did not exist.
#
# THIS SCRIPT asks the SERVER. A lane is live iff there is a child session of
# this session with NO outcome yet (`outcome` is null while a session runs and
# is set to "succeeded"/"error" when it ends).
#
#   scripts/pool_lanes.sh [--claims <path>] [--project <dir>] [--sessions <json>]
#
# It prints the live lanes and then a RECONCILIATION against the ledger:
#
#   PHANTOM   a `live` claim with NO running session   (the erroneous count)
#   UNCLAIMED a running session with NO `live` claim    (an unrecorded lane)
#
# Exit 0 when the two agree; exit 3 when they do not (so a heartbeat can act).
#
# NOTE: the ledger's last-wins key is (lane, issue) — a lane is cleared by
# appending a done-row under its ORIGINAL issue key, not a new one.

set -uo pipefail

CLAIMS="${BEZEL_STATUS_CLAIMS:-/var/tmp/bezel-scratch/r38-coord/claims.tsv}"
PROJECT="${BEZEL_PROJECT_DIR:-$PWD}"
SESSIONS=""
OPENCODE_BIN="${OPENCODE_BIN:-opencode}"
export PATH="${HOME}/.opencode/bin:${PATH}"

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --claims)   CLAIMS="$2"; shift 2 ;;
    --project)  PROJECT="$2"; shift 2 ;;
    --sessions) SESSIONS="$2"; shift 2 ;;
    -h|--help)  usage; exit 0 ;;
    *) echo "pool_lanes: unknown argument '$1'" >&2; exit 64 ;;
  esac
done

tmp=""
cleanup() { [ -n "$tmp" ] && rm -f "$tmp"; }
trap cleanup EXIT

if [ -z "$SESSIONS" ]; then
  tmp="$(mktemp)"
  if ! "$OPENCODE_BIN" api session.list --param "directory=$PROJECT" >"$tmp" 2>/dev/null; then
    echo "pool_lanes: could not reach the opencode server (opencode api session.list failed)" >&2
    exit 70
  fi
  SESSIONS="$tmp"
fi

python3 - "$SESSIONS" "$CLAIMS" <<'PY'
import json, os, subprocess, sys

sess_path, claims_path = sys.argv[1], sys.argv[2]

# The AGENT PROCESSES that are alive right now.  A lane is an `opencode run`
# process, so the process table is the DIRECT measure of "an agent exists" --
# the session's own timestamps are only a proxy (see below).
ps_ok = True
try:
    ps_out = subprocess.run(["ps", "-eo", "args="], capture_output=True,
                            text=True, timeout=30).stdout
except Exception:
    ps_out = ""
    ps_ok = False
agent_sessions = set()
for line in ps_out.splitlines():
    if "opencode run" not in line:
        continue
    parts = line.split()
    if "--session" in parts:
        i = parts.index("--session")
        if i + 1 < len(parts):
            agent_sessions.add(parts[i + 1])

try:
    with open(sess_path, encoding="utf-8") as fh:
        blob = json.load(fh)
except Exception as exc:
    print(f"pool_lanes: cannot parse the session list: {exc}", file=sys.stderr)
    sys.exit(70)
rows = blob.get("data", blob) if isinstance(blob, dict) else blob

# A lane is live iff ITS AGENT PROCESS IS ALIVE.
#
# WHY THE PROCESS, AND NOT THE SESSION (measured 2026-11-03, the R38 run).
# A lane IS an `opencode run --session <id>` process, so the process table is
# the DIRECT measure of "an agent exists".  The session's own fields are only
# a proxy, and the proxy is wrong in TWO measured ways:
#   * a RESUMED session keeps the OUTCOME OF ITS PREVIOUS RUN until the new
#     run ends, so `outcome is None` alone calls a RUNNING lane DEAD; and
#   * an agent whose PROCESS died leaves a session whose `outcome` is still
#     None and whose `time.idle` was never refreshed, so `outcome is None`
#     alone calls a DEAD lane LIVE.  Measured: 8 sessions read live while
#     only 6 agent processes existed -- two lanes had already finished and
#     the pool over-counted them.
# ⇒ SO: the process table decides.  A session with no agent process has no
# agent.  If the process table cannot be READ AT ALL (`ps` failed), fall back
# to the session fields -- never report a pool of zero because `ps` failed.
# ⇒ BUT an EMPTY PROCESS TABLE IS NOT A FAILURE: it means there is no agent
# right now, and the session fields would then OVER-count (a dead agent
# leaves a session whose `idle` was never refreshed).  So the fallback is
# taken only when `ps` FAILED, not when `ps` answered.
live = {}
if not ps_ok:
    for r in rows:
        if not r.get("parentID"):
            continue
        t = r.get("time") or {}
        if r.get("outcome") is None or ((t.get("updated") or 0) > (t.get("idle") or 0)):
            live[r["id"]] = (r.get("title") or "")[:60]
else:
    for r in rows:
        if not r.get("parentID"):
            continue
        if r["id"] in agent_sessions:
            live[r["id"]] = (r.get("title") or "")[:60]

print(f"pool_lanes: {len(live)} live agent session(s) per opencode")

# The ledger: last-wins per (lane, issue); a lane is live while it holds a
# `live` claim.
claims = {}
if os.path.exists(claims_path):
    with open(claims_path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 2 or parts[0] == "issue":
                continue
            claims[(parts[1], parts[0])] = parts[-1]

live_claims = {}
for (lane, issue), status in claims.items():
    if status.startswith("live"):
        live_claims[lane] = status

# The ledger now carries the session id in the status (status=live:…:ses_…).
claim_sessions = {}
for lane, status in live_claims.items():
    for tok in status.split(":"):
        if tok.startswith("ses_"):
            claim_sessions[lane] = tok

phantom = sorted(l for l in live_claims if l not in claim_sessions)
dead = sorted(l for l in live_claims
              if l in claim_sessions and claim_sessions[l] not in live)
unrecorded = sorted(
    sid for sid in live
    if not any(v == sid for v in claim_sessions.values())
)

for lane in sorted(live_claims):
    sid = claim_sessions.get(lane, "-")
    title = live.get(sid, "")
    if sid == "-":
        mark = "?? "          # a live claim with NO session id: unverifiable
    elif sid in live:
        mark = "OK "
    else:
        mark = "!! "          # the session ENDED: the lane is DEAD
    print(f"  {mark}{lane:<24} {sid:<28} {title}")

for lane in phantom:
    print(f"  PHANTOM   {lane}: a `live` claim with NO running session "
          f"— the claim is a promise, not an agent")
for lane in dead:
    print(f"  DEAD      {lane}: the claim says `live` but its session "
          f"({claim_sessions[lane]}) has ENDED — the lane died or handed "
          f"its turn back; re-dispatch it or close the claim")
for sid in unrecorded:
    print(f"  UNRECORDED {sid}: a running session with NO `live` claim "
          f"— {live.get(sid,'')}")

if phantom or dead or unrecorded:
    print("pool_lanes: MISMATCH — the ledger and opencode disagree")
    sys.exit(3)
print("pool_lanes: the ledger and opencode AGREE")
PY
