"""scripts/live_lanes.py against a fake /proc and a fake ~/.claude.

The measurement must count what RUNS, not what the ledger claims (R42,
2026-11-19: the ledger said 28 live lanes while 3 agents ran). Every input is
env-overridden (BEZEL_PROC_ROOT, BEZEL_CLAUDE_HOME, BEZEL_NOW), so the test
reads nothing outside its TemporaryDirectory.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

from orch_tools_support import SCRIPTS

TOOL = SCRIPTS / "live_lanes.py"
NOW = 2_000_000_000


class LiveLanesTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="live-lanes-")
        self.d = pathlib.Path(self._tmp.name)
        self.proc = self.d / "proc"
        self.claude = self.d / "claude"
        self.proc.mkdir()
        (self.claude / "sessions").mkdir(parents=True)
        self.env = dict(os.environ, BEZEL_PROC_ROOT=str(self.proc),
                        BEZEL_CLAUDE_HOME=str(self.claude), BEZEL_NOW=str(NOW))

    def tearDown(self):
        self._tmp.cleanup()

    # ---- fixtures ------------------------------------------------------
    def process(self, pid, argv, start="1000"):
        p = self.proc / str(pid)
        p.mkdir()
        (p / "cmdline").write_bytes(b"\0".join(a.encode() for a in argv) + b"\0")
        # field 2 (comm) deliberately contains a space and a ')' to pin the
        # parse of the fields after the LAST ')'.
        fields = ["S"] + ["0"] * 18 + [start] + ["0"] * 10
        (p / "stat").write_text("%d (we ird) x) %s\n" % (pid, " ".join(fields)))

    def session(self, pid, sid, start="1000"):
        (self.claude / "sessions" / ("%d.json" % pid)).write_text(
            json.dumps({"pid": pid, "sessionId": sid, "procStart": start}))

    def subagent(self, sid, aid, desc, finished=False, last=None, age_min=1):
        d = self.claude / "projects" / "-repo" / sid / "subagents"
        d.mkdir(parents=True, exist_ok=True)
        meta = {"description": desc, "model": "sonnet"}
        if finished:
            meta["finished"] = True
        (d / ("agent-%s.meta.json" % aid)).write_text(json.dumps(meta))
        tr = d / ("agent-%s.jsonl" % aid)
        entry = last or {"type": "assistant", "message": {"content": [{"type": "tool_use"}]}}
        tr.write_text(json.dumps({"type": "user"}) + "\n" + json.dumps(entry) + "\n")
        t = NOW - age_min * 60
        os.utime(tr, (t, t))

    def claims(self, *rows):
        p = self.d / "claims.tsv"
        p.write_text("issue\tlane\tbranch\tworktree\thot\tstart\tstatus\n" +
                     "".join("\t".join(r) + "\n" for r in rows))
        return str(p)

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *args], env=self.env,
                              capture_output=True, text=True, timeout=60)

    def measure(self, *args):
        r = self.run_tool("--json", *args)
        self.assertIn(r.returncode, (0, 3), r.stderr)
        return json.loads(r.stdout), r.returncode

    # ---- OpenCode ------------------------------------------------------
    def test_opencode_lane_is_an_opencode_run_process(self):
        self.process(10, ["opencode", "run", "--session", "ses_A1", "--title", "r42-m1-1"])
        self.process(11, ["/home/u/.opencode/bin/opencode", "run", "--session", "ses_B2"])
        j, _ = self.measure()
        self.assertEqual(j["opencode"], 2)
        self.assertEqual({l["name"] for l in j["lanes"]}, {"r42-m1-1", ""})

    def test_a_shell_line_mentioning_opencode_run_is_not_a_lane(self):
        self.process(12, ["bash", "-c", "pgrep -f 'opencode run --session ses_X'"])
        self.process(13, ["opencode", "serve"])
        j, _ = self.measure()
        self.assertEqual(j["opencode"], 0)

    def test_a_session_with_two_processes_counts_once(self):
        for pid in (14, 15):
            self.process(pid, ["opencode", "run", "--session", "ses_DUP"])
        j, _ = self.measure()
        self.assertEqual(j["opencode"], 1)

    # ---- Claude Code ---------------------------------------------------
    def test_unfinished_subagent_of_a_live_session_is_live(self):
        self.process(20, ["claude"], start="777")
        self.session(20, "S1", start="777")
        self.subagent("S1", "a" + "1" * 16, "Review #1")
        self.subagent("S1", "a" + "2" * 16, "Review #2", finished=True)
        j, _ = self.measure()
        self.assertEqual(j["claude"], 1)
        self.assertEqual(j["lanes"][0]["name"], "Review #1")

    def test_subagents_of_a_dead_session_are_not_live(self):
        # A crashed session leaves its unfinished metas forever.
        self.session(21, "S-dead")  # no /proc/21
        self.subagent("S-dead", "a" + "3" * 16, "orphan")
        j, _ = self.measure()
        self.assertEqual(j["claude"], 0)

    def test_a_reused_pid_is_not_the_session(self):
        self.process(22, ["bash"], start="999")
        self.session(22, "S-reused", start="111")
        self.subagent("S-reused", "a" + "4" * 16, "orphan")
        j, _ = self.measure()
        self.assertEqual(j["claude"], 0)

    def test_an_interrupted_subagent_is_not_live(self):
        self.process(23, ["claude"])
        self.session(23, "S2")
        self.subagent("S2", "a" + "5" * 16, "stopped", last={
            "type": "user", "message": {"content": [
                {"type": "text", "text": "[Request interrupted by user]"}]}})
        j, _ = self.measure()
        self.assertEqual(j["claude"], 0)

    def test_a_long_idle_subagent_is_live_but_flagged_stalled(self):
        self.process(24, ["claude"])
        self.session(24, "S3")
        aid = "a" + "6" * 16
        self.subagent("S3", aid, "slow", age_min=90)
        j, _ = self.measure("--stall-min", "60")
        self.assertEqual(j["claude"], 1)
        self.assertEqual(j["stalled"], [aid])

    # ---- the ledger ----------------------------------------------------
    def test_phantom_and_unrecorded_are_reported_and_exit_3(self):
        self.process(30, ["opencode", "run", "--session", "ses_LIVE"])
        self.process(31, ["claude"])
        self.session(31, "S4")
        dead, live_cc = "a" + "7" * 16, "a" + "8" * 16
        self.subagent("S4", live_cc, "unrecorded reviewer")
        c = self.claims(
            ("1", "laneA", "b", "w", "h", "t", "live:producing:ses_LIVE"),
            ("2", "laneB", "b", "w", "h", "t", "live:reviewing:" + dead),
            ("3", "laneC", "b", "w", "h", "t", "live:producing"),
        )
        j, rc = self.measure("--claims", c)
        self.assertEqual(rc, 3)
        self.assertEqual(j["total"], 2)
        self.assertEqual([p["lane"] for p in j["phantom"]], ["laneB"])
        self.assertEqual([u["lane"] for u in j["unverifiable"]], ["laneC"])
        self.assertEqual(j["unrecorded"], [live_cc])
        self.assertEqual(j["ledger_lanes"], 3)

    def test_a_superseded_live_row_is_not_a_phantom(self):
        self.process(32, ["opencode", "run", "--session", "ses_OK"])
        c = self.claims(
            ("1", "laneA", "b", "w", "h", "t", "live:producing:ses_GONE"),
            ("1", "laneA", "b", "w", "h", "t", "done:abc123"),
            ("2", "laneB", "b", "w", "h", "t", "live:producing:ses_OK"),
        )
        r = self.run_tool("--claims", c, "--summary")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(r.stdout.strip(),
                         "measured 1 live (claude 0, opencode 1) | ledger 1 lane(s): "
                         "phantom 0, unrecorded 0, no-id 0")

    def test_unreadable_proc_exits_70(self):
        self.env["BEZEL_PROC_ROOT"] = str(self.d / "missing")
        r = self.run_tool()
        self.assertEqual(r.returncode, 70)


if __name__ == "__main__":
    unittest.main()
