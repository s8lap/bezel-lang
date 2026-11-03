"""scripts/run_status.sh in a sandbox: every path it reads is env-overridden.

The machine-wide probes (jobserver unit, no_builders, lane_status) are omitted
with BEZEL_STATUS_SKIP so the test reads nothing outside its TemporaryDirectory.
"""
import os
import pathlib
import subprocess
import tempfile
import unittest

from orch_tools_support import SCRIPTS, WANT, Repo, git_env, run


def dead_pid():
    p = subprocess.Popen(["true"])
    p.wait()
    return p.pid


class RunStatusTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="run-status-")
        self.d = pathlib.Path(self._tmp.name)
        (self.d / "scratch" / "gate-cycle").mkdir(parents=True)
        (self.d / "scratch" / "gate-cycle" / "abc_cycle.log").write_text(
            "line one\n[gate_full_cycle 12:00:00] cycle COMPLETE: both oracles green\n")
        (self.d / "fast").mkdir()
        for s in ("aot-bincache", "aot-harness"):
            os.symlink(str(self.d / "fast" / s), str(self.d / "scratch" / s))
        (self.d / "meminfo").write_text("MemTotal: 33554432 kB\nMemAvailable: 20971520 kB\n")
        self.env = git_env(self.d)
        self.env.update({
            "BEZEL_SCRATCH": str(self.d / "scratch"),
            "BEZEL_GATE_PIDFILE": str(self.d / "scratch" / "gate.pid"),
            "BEZEL_GATE_DONEFILE": str(self.d / "scratch" / "gate.done"),
            "BEZEL_AOT_FAST_ROOT": str(self.d / "fast"),
            "BEZEL_MEMINFO": str(self.d / "meminfo"),
            "BEZEL_STATUS_SKIP": "jobserver builders lanes",
        })

    def tearDown(self):
        self._tmp.cleanup()

    def status(self, *args):
        r = run("run_status.sh", *args, cwd=self.d, env=self.env)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def row(self, out, name):
        for ln in out.splitlines():
            if ln.split()[:1] == [name]:
                return ln
        self.fail("no %s row in:\n%s" % (name, out))

    def test_quiet_sandbox_prints_ok_gate_row_and_no_actions(self):
        (self.d / "scratch" / "gate.done").write_text("0\n")
        out = self.status()
        gate = self.row(out, "gate")
        self.assertIn(" OK ", gate)
        self.assertIn("done: 0", gate)
        self.assertIn("abc_cycle.log: [gate_full_cycle 12:00:00] cycle COMPLETE", gate)
        self.assertIn(" OK ", self.row(out, "disk"))
        self.assertIn("MemAvailable 20.0G", self.row(out, "memory"))
        self.assertNotIn("ACTION:", out)

    def test_live_pidfile_reads_running(self):
        (self.d / "scratch" / "gate.pid").write_text("%d\n" % os.getpid())
        self.assertIn("RUNNING pid %d" % os.getpid(), self.row(self.status(), "gate"))

    def test_stale_pidfile_is_flagged_with_an_action(self):
        (self.d / "scratch" / "gate.pid").write_text("%d\n" % dead_pid())
        out = self.status()
        gate = self.row(out, "gate")
        self.assertIn("FLAG", gate)
        self.assertIn("STALE pidfile", gate)
        self.assertIn("ACTION: gate: pidfile", out)

    def test_bincache_as_real_dir_is_flagged(self):
        os.unlink(str(self.d / "scratch" / "aot-bincache"))
        (self.d / "scratch" / "aot-bincache").mkdir()
        out = self.status()
        self.assertIn("FLAG", self.row(out, "disk"))
        self.assertIn("aot-bincache is not a symlink", out)

    def test_staging_at_target_is_flagged(self):
        # THE FIXTURE IS ALSO A REGRESSION TEST FOR THE HOUSE RULE (measured
        # 2026-11-03, the R38 run). GitHub binds ONE closing keyword to ONE
        # reference, and the reference FOLLOWS the keyword -- IMMEDIATELY (only
        # whitespace and an optional `:` may sit between). So the subject
        # "fix #4201 and #4202" closes #4201 ONLY: #4202 does not follow the
        # keyword, so nothing closes it. The old fixture used exactly that
        # shape and measured the SUBJECT-ONLY count, which is why the row
        # counted issues that no push would ever close.
        #
        # => The second commit therefore carries a REAL closing keyword for its
        # second issue, on its own line, and the row measures the
        # GitHub-closable set.
        repo = Repo(self.d / "repo", self.env)
        repo.git("update-ref", "refs/remotes/origin/main", "HEAD")
        repo.change("a", "fix #4200\n\n" + WANT)
        repo.change("b", "fix #4201\n\n" + WANT + "\nFixes #4202\n")
        out = self.status("--staging", str(repo.root), "--target", "3")
        st = self.row(out, "staging")
        self.assertIn("FLAG", st)
        self.assertIn("+2 over origin/main | closable 3/3", st)
        self.assertIn("ACTION: staging: 3 issues GitHub would close", out)
        self.assertIn(" OK ", self.row(self.status("--staging", str(repo.root)), "staging"))


    def test_inactive_jobserver_is_info_without_an_action(self):
        # A fake systemctl reports the unit inactive; the jobserver is opt-in.
        fake = self.d / "fakebin"
        fake.mkdir()
        (fake / "systemctl").write_text("#!/bin/sh\necho inactive\nexit 3\n")
        (fake / "systemctl").chmod(0o755)
        self.env["PATH"] = str(fake) + os.pathsep + self.env["PATH"]
        self.env["BEZEL_STATUS_SKIP"] = "builders lanes"
        out = self.status()
        js = self.row(out, "jobserver")
        self.assertIn(" INFO ", js)
        self.assertIn("inactive", js)
        self.assertNotIn("ACTION: jobserver", out)

    def test_bad_arguments_exit_64_instead_of_looping(self):
        for argv in (["--target"], ["--staging"], ["--target", "abc"], ["--target", "-1"],
                     ["--pool"], ["--claims"], ["--pool", "x"]):
            r = subprocess.run([str(SCRIPTS / "run_status.sh")] + argv, cwd=str(self.d),
                               env=self.env, capture_output=True, text=True, timeout=20)
            self.assertEqual(r.returncode, 64, argv)

    # ---- the pool row -------------------------------------------------
    #
    # The pool is a FIRM size, refilled in the same turn a slot frees, and
    # until 2026-11-02 nothing could see it: a run left a slot empty for
    # nineteen minutes and no row said so. The pool row counts the DISTINCT
    # lanes holding a live claim (one lane fixing three issues is one lane),
    # flags a PARTIAL pool, and stays quiet for a DRAINED one (which is what a
    # gate wants).

    def claims(self, *rows):
        p = self.d / "claims.tsv"
        p.write_text("issue\tlane\tbranch\tworktree\thot\tstart\tstatus\n" +
                     "".join("\t".join(r) + "\n" for r in rows))
        return str(p)

    def test_pool_counts_distinct_lanes_not_claims(self):
        c = self.claims(
            ("#1", "laneA", "b1", "/w/a", "f", "t", "live:producing"),
            ("#2", "laneA", "b1", "/w/a", "f", "t", "live:producing"),
            ("#3", "laneB", "b2", "/w/b", "f", "t", "live:reviewing"),
        )
        out = self.status("--claims", c, "--pool", "2")
        pool = self.row(out, "pool")
        self.assertIn(" OK ", pool)
        self.assertIn("2 live lane(s) of 2", pool)
        self.assertNotIn("ACTION: pool", out)

    def test_partial_pool_is_flagged_with_an_action(self):
        c = self.claims(("#1", "laneA", "b1", "/w/a", "f", "t", "live:producing"))
        out = self.status("--claims", c, "--pool", "7")
        self.assertIn("FLAG", self.row(out, "pool"))
        self.assertIn("1 live lane(s) of 7", self.row(out, "pool"))
        self.assertIn("ACTION: pool:", out)

    def test_a_queued_claim_is_not_a_live_lane(self):
        # A lane that also holds a QUEUED claim for a later issue is still
        # live -- which is why live rows are counted and queued ones ignored,
        # rather than one status per lane.
        c = self.claims(
            ("#1", "laneA", "b1", "/w/a", "f", "t", "live:producing"),
            ("#2", "laneA", "b1", "/w/a", "f", "t", "queued:waiting on another lane"),
        )
        out = self.status("--claims", c, "--pool", "1")
        self.assertIn("1 live lane(s) of 1", self.row(out, "pool"))
        self.assertNotIn("ACTION: pool", out)

    def test_a_superseded_claim_does_not_keep_a_lane_live(self):
        # "LAST ROW WINS" IS PER CLAIM, NOT PER LANE. A lane whose only claim
        # was later marked done is NOT live -- otherwise a finished lane stays
        # counted forever and the pool row over-reports.
        c = self.claims(
            ("#1", "laneA", "b1", "/w/a", "f", "t", "live:producing"),
            ("#1", "laneA", "b1", "/w/a", "f", "t", "done:cc8d3170"),
        )
        out = self.status("--claims", c, "--pool", "7")
        self.assertIn("0 live lane(s) of 7", self.row(out, "pool"))
        self.assertNotIn("ACTION: pool", out)

    def test_one_lane_two_issues_counts_once(self):
        c = self.claims(
            ("#1", "laneA", "b1", "/w/a", "f", "t", "live:producing"),
            ("#2", "laneA", "b1", "/w/a", "f", "t", "live:producing"),
            ("#3", "laneB", "b2", "/w/b", "f", "t", "live:reviewing"),
        )
        out = self.status("--claims", c, "--pool", "7")
        self.assertIn("2 live lane(s) of 7", self.row(out, "pool"))

    def test_drained_pool_is_info_not_an_action(self):
        # A drained pool is legitimate -- it is what a gate wants -- so 0
        # lanes must not cry for a refill.
        c = self.claims(("#1", "laneA", "b1", "/w/a", "f", "t", "done"))
        out = self.status("--claims", c, "--pool", "7")
        self.assertIn(" INFO ", self.row(out, "pool"))
        self.assertIn("0 live lane(s) of 7", self.row(out, "pool"))
        self.assertNotIn("ACTION: pool", out)

    def test_missing_claims_file_is_info_not_a_crash(self):
        out = self.status("--claims", str(self.d / "nope.tsv"), "--pool", "7")
        self.assertIn(" INFO ", self.row(out, "pool"))
        self.assertNotIn("ACTION: pool", out)

if __name__ == "__main__":
    unittest.main()
