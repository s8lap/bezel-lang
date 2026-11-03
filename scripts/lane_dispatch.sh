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
#
# PRINTS the new session id on stdout (and nothing else), so the caller can
# record it in claims.tsv as `live:<role>:ses_...`.
#
# EXIT: 0 dispatched · 64 usage/refusal · 70 the server was unreachable.
set -u

TITLE=""; LOG=""; BRIEF_FILE=""; MODEL="minimax/MiniMax-M3.1-Flash"; AGENT="general"
BRIEF=""; PROJECT="${OPENCODE_PROJECT:-/home/stepan/w/code/bezel-lang}"

die() { echo "lane_dispatch: $*" >&2; exit 64; }

while [ $# -gt 0 ]; do
  case "$1" in
    --title)      TITLE="${2:-}"; shift 2 ;;
    --log)        LOG="${2:-}"; shift 2 ;;
    --brief-file) BRIEF_FILE="${2:-}"; shift 2 ;;
    --model)      MODEL="${2:-}"; shift 2 ;;
    --agent)      AGENT="${2:-}"; shift 2 ;;
    --project)    PROJECT="${2:-}"; shift 2 ;;
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
create_out="$(opencode api session.create \
  --data "{\"title\":\"$TITLE\",\"parentID\":\"$COORD\"}" 2>&1)" || {
  echo "lane_dispatch: session.create failed: $create_out" >&2; exit 70; }
SID="$(printf '%s' "$create_out" | python3 -c \
  'import json,sys
try: print(json.load(sys.stdin)["data"]["id"])
except Exception: print("")' 2>/dev/null)"
[ -n "$SID" ] || { echo "lane_dispatch: cannot parse the session id: $create_out" >&2; exit 70; }

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
