#!/usr/bin/env python3
"""live_lanes.py -- MEASURE the live lanes across Claude Code AND OpenCode.

WHY THIS EXISTS. `run_status.sh`'s `pool` row counted the claims ledger, and a
ledger row is a CLAIM, not an agent. Measured 2026-11-19 (R42): the pool row
said "28 live lane(s)" while exactly 3 agents ran (1 Claude Code subagent, 2
OpenCode lanes): stale `live` rows under old (lane, issue) keys are never
superseded. `pool_lanes.sh` measures OpenCode only, and through the opencode
CLI, which can kill a stalled server (R42 memory: every reset followed a CLI
call during a 40-60 s stall). Claude Code subagents were not measured at all.

THIS SCRIPT READS ONLY LOCAL STATE -- no CLI call, no network:

  OpenCode   a lane IS an `opencode run --session ses_... [--title T]`
             process. argv[0]'s basename must be `opencode` and argv[1] `run`,
             so a shell line that merely MENTIONS the words is not an agent.

  Claude     a live Claude Code session has `<claude>/sessions/<pid>.json`
             ({"pid", "sessionId", "procStart"}). It is live iff that pid is
             alive AND its /proc starttime equals procStart (pid reuse).
             Its subagents are `<claude>/projects/*/<sessionId>/subagents/
             agent-<id>.meta.json`; one is live iff the meta has no
             `"finished": true` and its transcript's last entry is not the
             harness's "[Request interrupted ..." marker. A subagent of a
             DEAD session is never live (an unfinished meta from a crashed
             session stays unfinished forever).

  Ledger     claims.tsv, last row wins per (lane, issue), as run_status reads
             it. A live row may carry the agent id: `live:<role>:<id>`.

Reconciliation:
  PHANTOM     a ledger-live row whose id is a measured-dead agent, or a
              ledger-live row with no id (unverifiable -- listed separately)
  UNRECORDED  a measured-live agent whose id no ledger-live row carries

  scripts/live_lanes.py [--claims P] [--stall-min N] [--summary | --json]

Exit 0 when ledger and measurement agree, 3 when they do not, 70 when the
process table cannot be read. `--summary` prints one line (for run_status).
Overrides for tests: BEZEL_PROC_ROOT (default /proc), BEZEL_CLAUDE_HOME
(default ~/.claude), BEZEL_NOW (epoch seconds).
"""
import argparse
import glob
import json
import os
import re
import sys
import time

PROC = os.environ.get("BEZEL_PROC_ROOT", "/proc")
CLAUDE = os.environ.get("BEZEL_CLAUDE_HOME", os.path.expanduser("~/.claude"))
NOW = float(os.environ.get("BEZEL_NOW") or time.time())
INTERRUPTED = "[Request interrupted"
ID_RE = re.compile(r"^(ses_[A-Za-z0-9]+|a[0-9a-f]{16})$")


def pids():
    try:
        names = os.listdir(PROC)
    except OSError:
        return None
    return [n for n in names if n.isdigit()]


def argv(pid):
    try:
        with open(os.path.join(PROC, pid, "cmdline"), "rb") as fh:
            raw = fh.read()
    except OSError:
        return []
    return [a.decode("utf-8", "replace") for a in raw.split(b"\0") if a]


def starttime(pid):
    try:
        with open(os.path.join(PROC, str(pid), "stat")) as fh:
            s = fh.read()
    except OSError:
        return None
    # comm (field 2) may contain spaces; fields after the last ')' start at 3.
    rest = s[s.rfind(")") + 2:].split()
    return rest[19] if len(rest) > 19 else None  # field 22


def opencode_lanes(all_pids):
    lanes = {}
    for pid in all_pids:
        a = argv(pid)
        if len(a) < 3 or os.path.basename(a[0]) != "opencode" or a[1] != "run":
            continue
        if "--session" not in a:
            continue
        i = a.index("--session")
        if i + 1 >= len(a):
            continue
        sid = a[i + 1]
        title = a[a.index("--title") + 1] if "--title" in a and a.index("--title") + 1 < len(a) else ""
        # A resumed lane can briefly have two processes; count the session once.
        lanes.setdefault(sid, {"kind": "opencode", "id": sid, "name": title,
                               "pid": int(pid), "idle_min": None})
    return lanes


def last_entry(path):
    try:
        with open(path, "rb") as fh:
            fh.seek(0, 2)
            size = fh.tell()
            fh.seek(max(0, size - 65536))
            tail = fh.read().splitlines()
    except OSError:
        return None
    for ln in reversed(tail):
        try:
            return json.loads(ln)
        except ValueError:
            continue
    return None


def is_interrupted(entry):
    if not entry or entry.get("type") != "user":
        return False
    c = (entry.get("message") or {}).get("content")
    if isinstance(c, str):
        return c.startswith(INTERRUPTED)
    if isinstance(c, list):
        return any(isinstance(x, dict) and str(x.get("text", "")).startswith(INTERRUPTED) for x in c)
    return False


def claude_sessions():
    live = []
    for f in glob.glob(os.path.join(CLAUDE, "sessions", "*.json")):
        try:
            with open(f) as fh:
                d = json.load(fh)
        except (OSError, ValueError):
            continue
        pid, sid = d.get("pid"), d.get("sessionId")
        if not pid or not sid:
            continue
        st = starttime(pid)
        if st is None:
            continue
        if d.get("procStart") is not None and str(d["procStart"]) != st:
            continue  # the pid was reused by another process
        live.append(sid)
    return live


def claude_lanes(sessions):
    lanes = {}
    for sid in sessions:
        for meta in glob.glob(os.path.join(CLAUDE, "projects", "*", sid, "subagents", "agent-*.meta.json")):
            try:
                with open(meta) as fh:
                    m = json.load(fh)
            except (OSError, ValueError):
                continue
            if m.get("finished") is True:
                continue
            tr = meta[: -len(".meta.json")] + ".jsonl"
            if is_interrupted(last_entry(tr)):
                continue
            aid = os.path.basename(tr)[len("agent-"):-len(".jsonl")]
            try:
                idle = (NOW - os.stat(tr).st_mtime) / 60.0
            except OSError:
                idle = None
            lanes[aid] = {"kind": "claude", "id": aid, "name": m.get("description", ""),
                          "model": m.get("model", ""), "session": sid,
                          "idle_min": None if idle is None else round(idle, 1)}
    return lanes


def ledger(path):
    """Last row wins per (lane, issue); returns the live rows."""
    st = {}
    try:
        with open(path, encoding="utf-8") as fh:
            for ln in fh:
                f = ln.rstrip("\n").split("\t")
                if len(f) < 2 or f[0] == "issue":
                    continue
                st[(f[1], f[0])] = f
    except OSError:
        return None
    rows = []
    for (lane, issue), f in st.items():
        status = f[-1]
        if not status.startswith("live"):
            continue
        parts = status.split(":")
        aid = parts[2] if len(parts) > 2 and ID_RE.match(parts[2]) else None
        rows.append({"lane": lane, "issue": issue, "id": aid})
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--claims", default=os.environ.get("BEZEL_STATUS_CLAIMS", ""))
    ap.add_argument("--stall-min", type=float, default=60.0,
                    help="flag a Claude subagent whose transcript is idle this long")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--summary", action="store_true")
    g.add_argument("--json", action="store_true")
    a = ap.parse_args()

    ps = pids()
    if ps is None:
        print("live_lanes: cannot read the process table at %s" % PROC, file=sys.stderr)
        return 70
    oc = opencode_lanes(ps)
    cc = claude_lanes(claude_sessions())
    live = dict(cc)
    live.update(oc)
    stalled = sorted(k for k, v in cc.items() if v["idle_min"] is not None and v["idle_min"] >= a.stall_min)

    rows = ledger(a.claims) if a.claims else None
    phantom, unverifiable, unrecorded = [], [], []
    if rows is not None:
        ids = {r["id"] for r in rows if r["id"]}
        for r in rows:
            if r["id"] is None:
                unverifiable.append(r)
            elif r["id"] not in live:
                phantom.append(r)
        unrecorded = sorted(k for k in live if k not in ids)
    ledger_lanes = None if rows is None else len({r["lane"] for r in rows})
    agree = rows is None or (not phantom and not unrecorded)

    if a.json:
        print(json.dumps({"claude": len(cc), "opencode": len(oc), "total": len(live),
                          "lanes": sorted(live.values(), key=lambda v: (v["kind"], v["name"])),
                          "stalled": stalled, "ledger_lanes": ledger_lanes,
                          "phantom": phantom, "unverifiable": unverifiable,
                          "unrecorded": unrecorded}, indent=1))
    elif a.summary:
        s = "measured %d live (claude %d, opencode %d)" % (len(live), len(cc), len(oc))
        if rows is not None:
            s += " | ledger %d lane(s): phantom %d, unrecorded %d, no-id %d" % (
                ledger_lanes, len(phantom), len(unrecorded), len(unverifiable))
        if stalled:
            s += " | stalled %d" % len(stalled)
        print(s)
    else:
        print("live_lanes: %d live agent(s) -- claude %d, opencode %d" % (len(live), len(cc), len(oc)))
        for v in sorted(live.values(), key=lambda v: (v["kind"], v["name"])):
            idle = "" if v["idle_min"] is None else "  idle %.0fm%s" % (
                v["idle_min"], "  STALLED?" if v["id"] in stalled else "")
            print("  %-8s %-34s %s%s" % (v["kind"], v["id"], v["name"][:60], idle))
        if rows is not None:
            print("ledger: %d lane(s) hold a live row" % ledger_lanes)
            for r in phantom:
                print("  PHANTOM     %s (%s) id %s has no running agent -- append a done row under issue %s"
                      % (r["lane"], r["issue"], r["id"], r["issue"]))
            for r in unverifiable:
                print("  NO-ID       %s (%s) live row carries no agent id -- cannot be verified"
                      % (r["lane"], r["issue"]))
            for k in unrecorded:
                print("  UNRECORDED  %s %s (%s) runs with no live row" % (live[k]["kind"], k, live[k]["name"][:50]))
    return 0 if agree else 3


if __name__ == "__main__":
    sys.exit(main())
