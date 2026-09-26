"""scripts/run_status.sh in a sandbox: every path it reads is env-overridden.

The machine-wide probes (jobserver unit, no_builders, lane_status) are omitted
with BEZEL_STATUS_SKIP so the test reads nothing outside its TemporaryDirectory.
"""
import os
import pathlib
import subprocess
import tempfile
import unittest

from orch_tools_support import WANT, Repo, git_env, run


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
        repo = Repo(self.d / "repo", self.env)
        repo.git("update-ref", "refs/remotes/origin/main", "HEAD")
        repo.change("a", "fix #4200\n\n" + WANT)
        repo.change("b", "fix #4201 and #4202\n\n" + WANT)
        out = self.status("--staging", str(repo.root), "--target", "3")
        st = self.row(out, "staging")
        self.assertIn("FLAG", st)
        self.assertIn("+2 over origin/main | banked 3/3", st)
        self.assertIn("ACTION: staging: 3 fixes banked", out)
        self.assertIn(" OK ", self.row(self.status("--staging", str(repo.root)), "staging"))


if __name__ == "__main__":
    unittest.main()
