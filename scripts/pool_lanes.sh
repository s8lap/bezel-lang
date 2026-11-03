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
import json, os, sys

sess_path, claims_path = sys.argv[1], sys.argv[2]

try:
    with open(sess_path, encoding="utf-8") as fh:
        blob = json.load(fh)
except Exception as exc:
    print(f"pool_lanes: cannot parse the session list: {exc}", file=sys.stderr)
    sys.exit(70)
rows = blob.get("data", blob) if isinstance(blob, dict) else blob

# A lane is live iff it is a CHILD session with no outcome yet.
live = {}
for r in rows:
    if not r.get("parentID"):
        continue                       # a top-level session is the coordinator
    if r.get("outcome") is None:
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
