#!/usr/bin/env bash
# lane_dispatch.sh — launch a lane agent WITHOUT blocking the coordinator's turn.
#
# WHY THIS EXISTS (measured 2026-11-03, the R38 run).
#
# The harness's `subagent` tool returns a session id when called with
# `background: true`, but the TURN still waited for the agent: the
# coordinator's next tool call did not run until the agent finished. The pool
# row read 7 (it counts CLAIMS) while ONE agent was actually running, and the
# box was idle. Issuing several `subagent` calls in one message did not help —
# the harness ran the first one in the foreground and blocked on it.
#
# ⇒ THE FIX IS TO GO THROUGH THE SERVER DIRECTLY:
#   1. `opencode api session.create` makes a CHILD session of the coordinator
#      (parentID = $OPENCODE_SESSION_ID), so the reconciler sees it exactly
#      like a harness-spawned lane.
#   2. `opencode run --session <id> ...` executes it, DETACHED via setsid, so
#      this script returns in milliseconds and the coordinator's turn goes on.
#
# ⇒ THE MODEL IS SET EXPLICITLY (`--model`), because a session created through
#   the API and prompted with a bare body uses the server's DEFAULT agent and
#   model — measured: `build`/`opencode`/`pulse-alpha-free`, which is NOT the
#   campaign's model. The house model is minimax/MiniMax-M3.1-Flash and the
#   house agent is `general`.
#
# USAGE
#   scripts/lane_dispatch.sh --title "<title>" --log <path> [--model <m>]
#                          [--agent <a>] -- <brief text>
#   scripts/lane_dispatch.sh --title "<title>" --log <path> --brief-file <path>
#   scripts/lane_dispatch.sh --resume <session-id> --title "<title>" --log <path>
#                          --brief-file <path>
#
# --resume <session-id> CONTINUES an existing child session instead of
# creating a new one.  Use it when a lane's agent already exists and only
# needs a FOLLOW-UP (a scoped re-review of a fix-forward, say): the session
# keeps its history, so the agent still knows what it reviewed, and it stays
# a child of this coordinator session, so the reconciler counts it unchanged.
# The session MUST already be a child of $OPENCODE_SESSION_ID.
#
# PRINTS the session id on stdout (and nothing else), so the caller can
# record it in claims.tsv as `live:<role>:ses_...`.
#
# EXIT: 0 dispatched · 64 usage/refusal · 70 the server was unreachable.
set -u

TITLE=""; LOG=""; BRIEF_FILE=""; MODEL="minimax/MiniMax-M3.1-Flash#low"; AGENT="general"
# The project root is DERIVED FROM THIS SCRIPT'S OWN LOCATION, never a literal
# checkout path: a hardcoded absolute path is the defect the gate's
# repo-identity self-test (scripts/gate/tests/test_repo_identity.py,
# #3413) fails a cycle for -- and this script had one, so the gate's own
# self-test phase was RED on it.  The same rule scripts/ratchet_sweep.sh
# follows ("REPO-relative, never an absolute checkout path").
_SELF_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BRIEF=""; PROJECT="${OPENCODE_PROJECT:-$(cd -- "$_SELF_DIR/.." && pwd)}"; RESUME=""

die() { echo "lane_dispatch: $*" >&2; exit 64; }

while [ $# -gt 0 ]; do
  case "$1" in
    --title)      TITLE="${2:-}"; shift 2 ;;
    --log)        LOG="${2:-}"; shift 2 ;;
    --brief-file) BRIEF_FILE="${2:-}"; shift 2 ;;
    --model)      MODEL="${2:-}"; shift 2 ;;
    --agent)      AGENT="${2:-}"; shift 2 ;;
    --project)    PROJECT="${2:-}"; shift 2 ;;
    --resume)     RESUME="${2:-}"; shift 2 ;;
    --)           shift; BRIEF="$*"; break ;;
    *)            die "unknown argument '$1'" ;;
  esac
done

[ -n "$TITLE" ] || die "--title is required"
[ -n "$LOG" ]   || die "--log is required (the agent's transcript goes there)"
[ -n "$BRIEF" ] || [ -n "$BRIEF_FILE" ] || die "a brief is required (after -- or --brief-file)"

COORD="${OPENCODE_SESSION_ID:-}"
[ -n "$COORD" ] || die "OPENCODE_SESSION_ID is unset — this script must run inside an opencode session"

if [ -n "$BRIEF_FILE" ]; then
  [ -r "$BRIEF_FILE" ] || die "cannot read the brief file '$BRIEF_FILE'"
  BRIEF="$(cat "$BRIEF_FILE")"
fi

mkdir -p "$(dirname "$LOG")" || die "cannot create the log's directory"

# 1. the child session of THIS coordinator session
if [ -n "$RESUME" ]; then
  # CONTINUE an existing child session (a scoped re-review, say).  It must
  # already be a child of this coordinator, or the reconciler will not count
  # it and the UI will not show it under this session.
  SID="$RESUME"
  parent="$(opencode api session.list --param "directory=$PROJECT" 2>/dev/null \
    | python3 -c \
      'import json,sys
d=json.load(sys.stdin); rows=d.get("data") if isinstance(d,dict) else d
for r in rows:
    if (r.get("id") or "") == sys.argv[1]:
        print(r.get("parentID") or ""); break' \
      "$SID" 2>/dev/null)"
  [ -n "$parent" ] || { echo "lane_dispatch: --resume session $SID is not known to this project" >&2; exit 64; }
  [ "$parent" = "$COORD" ] || {
    echo "lane_dispatch: --resume session $SID is not a child of this coordinator ($COORD)" >&2
    exit 64; }
else
  create_out="$(opencode api session.create \
    --data "{\"title\":\"$TITLE\",\"parentID\":\"$COORD\"}" 2>&1)" || {
    echo "lane_dispatch: session.create failed: $create_out" >&2; exit 70; }
  SID="$(printf '%s' "$create_out" | python3 -c \
    'import json,sys
try: print(json.load(sys.stdin)["data"]["id"])
except Exception: print("")' 2>/dev/null)"
  [ -n "$SID" ] || { echo "lane_dispatch: cannot parse the session id: $create_out" >&2; exit 70; }
fi

# 2. run it DETACHED: the coordinator's turn continues immediately.
setsid nohup opencode run --session "$SID" --model "$MODEL" --agent "$AGENT" \
  --title "$TITLE" "$BRIEF" >"$LOG" 2>&1 </dev/null &
disown 2>/dev/null || true

# 3. the agent must actually EXIST — a claim is a promise, not an agent.
#    Give the server a moment to register the session, then confirm it.
for _ in 1 2 3 4 5 6 7 8 9 10; do
  sleep 0.4
  if opencode api session.list --param "directory=$PROJECT" 2>/dev/null \
      | python3 -c \
        'import json,sys
d=json.load(sys.stdin); rows=d if isinstance(d,list) else d.get("data") or []
sys.exit(0 if any((r.get("id") or "").endswith(sys.argv[1]) for r in rows) else 1)' \
        "${SID: -12}" 2>/dev/null; then
    echo "$SID"
    exit 0
  fi
done

echo "lane_dispatch: dispatched $SID but the server does not list it yet; check $LOG" >&2
echo "$SID"
exit 0
