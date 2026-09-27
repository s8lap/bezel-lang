"""Tests for scripts/build_jobserver.py (the machine-wide build token pool).

Pin groups:
  1. rewrite-args — every -j/--parallel spelling is stripped (an explicit -jN
     makes GNU make ignore the inherited pool), nothing else is touched.
  2. The cap — two real `make` processes joined to a 2-token pool never run
     more than 2 + 2 (implicit) jobs at once, and both finish.
  3. Self-repair — a token lost outside make is restored once the pool is idle;
     an over-returned token is drained back to exactly N.
  4. Busy surplus — stray tokens are drained back to N while a make still runs.
  5. Liveness — `alive` is false without a daemon; a second daemon on the same
     dir is refused (two holders would each publish N).
  6. `status` — reports `held` and a freshly-measured `clang` count on ONE
     line (non-author review: an orchestrator heartbeat must be able to spot
     a leak — held vs live clang — at a glance); fails cleanly with no daemon.
Everything runs in a TemporaryDirectory; nothing touches the production pool.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "..", "build_jobserver.py")
sys.path.insert(0, os.path.join(HERE, "..", ".."))

import build_jobserver as bj  # noqa: E402


def _avail(d):
    fd = os.open(os.path.join(d, "fifo"), os.O_RDONLY | os.O_NONBLOCK)
    try:
        return bj.available_tokens(fd)
    finally:
        os.close(fd)


def _wait_for(pred, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return pred()


class RewriteArgsTest(unittest.TestCase):
    def test_strips_every_parallel_spelling(self):
        self.assertEqual(
            bj.rewrite_args(["cmake", "--build", "b", "-j16", "--target", "t"]),
            ["cmake", "--build", "b", "--target", "t"])
        self.assertEqual(bj.rewrite_args(["cmake", "--build", "b", "-j", "8"]),
                         ["cmake", "--build", "b"])
        self.assertEqual(bj.rewrite_args(["cmake", "--build", "b", "--parallel", "4"]),
                         ["cmake", "--build", "b"])
        self.assertEqual(bj.rewrite_args(["cmake", "--build", "b", "--parallel=4"]),
                         ["cmake", "--build", "b"])
        self.assertEqual(bj.rewrite_args(["make", "--jobs=3", "all"]), ["make", "all"])
        self.assertEqual(bj.rewrite_args(["cmake", "--build", "b", "--", "-j12"]),
                         ["cmake", "--build", "b", "--"])

    def test_bare_j_does_not_eat_a_non_numeric_next_arg(self):
        self.assertEqual(bj.rewrite_args(["make", "-j", "all"]), ["make", "all"])

    def test_leaves_other_arguments_alone(self):
        args = ["cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                "--target", "bezel_tests_sema", "-jobs-like-name"]
        self.assertEqual(bj.rewrite_args(args), args)

    def test_cli_is_nul_separated(self):
        out = subprocess.run([sys.executable, SCRIPT, "rewrite-args", "--",
                              "cmake", "--build", "a b", "-j16"],
                             capture_output=True, check=True).stdout
        self.assertEqual(out.split(b"\0")[:-1], [b"cmake", b"--build", b"a b"])


class DaemonTestBase(unittest.TestCase):
    TOKENS = 2

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="bezel-js-test-")
        self.proc = subprocess.Popen(
            [sys.executable, SCRIPT, "serve", "--dir", self.dir,
             "--tokens", str(self.TOKENS), "--interval", "0.2", "--confirm", "2"])
        self.assertTrue(_wait_for(lambda: bj.alive(self.dir)), "daemon never came up")

    def tearDown(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


@unittest.skipUnless(shutil.which("make"), "GNU make not installed")
class CapTest(DaemonTestBase):
    def test_two_makes_share_the_pool(self):
        log = os.path.join(self.dir, "jobs.log")
        mk = os.path.join(self.dir, "Makefile")
        with open(mk, "w") as f:
            f.write("all: " + " ".join(f"t{i}" for i in range(8)) + "\n"
                    "t%:\n"
                    f"\t@echo \"+ $$(date +%s.%N)\" >> {log}; sleep 0.4; "
                    f"echo \"- $$(date +%s.%N)\" >> {log}\n")
        env = dict(os.environ, MAKEFLAGS=f"-j --jobserver-auth=fifo:{self.dir}/fifo")
        ps = [subprocess.Popen(["make", "-s", "-f", mk], env=env, cwd=self.dir)
              for _ in range(2)]
        for p in ps:
            self.assertEqual(p.wait(timeout=60), 0)
        with open(log) as f:
            events = sorted((float(t), 1 if s == "+" else -1)
                            for s, t in (line.split() for line in f))
        running = peak = 0
        for _, delta in events:
            running += delta
            peak = max(peak, running)
        self.assertEqual(sum(1 for _, d in events if d == 1), 16)
        self.assertLessEqual(peak, self.TOKENS + 2)
        self.assertGreaterEqual(peak, 2, "the pool never ran jobs in parallel")
        # Every token came home.
        self.assertTrue(_wait_for(lambda: _avail(self.dir) == self.TOKENS))


NINJA = shutil.which("ninja-build") or shutil.which("ninja")


@unittest.skipUnless(NINJA, "ninja not installed")
class NinjaCapTest(DaemonTestBase):
    """Two ninja builds joined to the pool obey the same cap as makes.

    Fedora installs ninja as `ninja-build`, so that is the holder's comm. A
    holder the audit does not recognise has its held tokens read as a leak
    and re-minted, which made the pool unbounded (R22, 2026-09-27: ~245
    concurrent compiles against 20 tokens). Jobs are long enough to span
    several audit/confirm cycles, so a mis-counted holder does get repaired."""

    def test_two_ninjas_share_the_pool(self):
        log = os.path.join(self.dir, "jobs.log")
        for n in range(2):
            sub = os.path.join(self.dir, f"n{n}")
            os.mkdir(sub)
            with open(os.path.join(sub, "build.ninja"), "w") as f:
                f.write("rule s\n"
                        f"  command = echo \"+ $$(date +%s.%N)\" >> {log}; sleep 1.2; "
                        f"echo \"- $$(date +%s.%N)\" >> {log}; touch $out\n")
                for i in range(8):
                    f.write(f"build o{i}: s\n")
        env = dict(os.environ, MAKEFLAGS=f"-j --jobserver-auth=fifo:{self.dir}/fifo")
        ps = [subprocess.Popen([NINJA, "-C", os.path.join(self.dir, f"n{n}")],
                               env=env, stdout=subprocess.DEVNULL)
              for n in range(2)]
        for p in ps:
            self.assertEqual(p.wait(timeout=120), 0)
        with open(log) as f:
            events = sorted((float(t), 1 if s == "+" else -1)
                            for s, t in (line.split() for line in f))
        running = peak = 0
        for _, delta in events:
            running += delta
            peak = max(peak, running)
        self.assertEqual(sum(1 for _, d in events if d == 1), 16)
        self.assertLessEqual(peak, self.TOKENS + 2,
                             "ninja's held tokens were re-minted as a leak")
        self.assertTrue(_wait_for(lambda: _avail(self.dir) == self.TOKENS))


class RepairTest(DaemonTestBase):
    def test_lost_token_is_restored_when_idle(self):
        fd = os.open(os.path.join(self.dir, "fifo"), os.O_RDONLY | os.O_NONBLOCK)
        try:
            self.assertEqual(os.read(fd, 1), b"+")  # "leak" one: nobody returns it
        finally:
            os.close(fd)
        self.assertTrue(_wait_for(lambda: _avail(self.dir) == self.TOKENS),
                        "a lost token was never restored")

    def test_over_return_is_drained_to_n(self):
        fd = os.open(os.path.join(self.dir, "fifo"), os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(fd, b"++")
        finally:
            os.close(fd)
        self.assertTrue(_wait_for(lambda: _avail(self.dir) == self.TOKENS),
                        "an over-returned token was never drained")


@unittest.skipUnless(shutil.which("make"), "GNU make not installed")
class BusySurplusTest(DaemonTestBase):
    def test_surplus_is_drained_while_a_make_is_running(self):
        # A daemon restarted under live builds publishes a fresh N while the
        # old holders still return theirs. Model it: one make holding the
        # FIFO with a long job, plus two stray tokens. The pool must come back
        # to N while that make is still running, not only once it is idle.
        mk = os.path.join(self.dir, "Makefile")
        with open(mk, "w") as f:
            f.write("all:\n\t@sleep 6\n")
        env = dict(os.environ, MAKEFLAGS=f"-j --jobserver-auth=fifo:{self.dir}/fifo")
        p = subprocess.Popen(["make", "-s", "-f", mk], env=env, cwd=self.dir)
        try:
            self.assertTrue(_wait_for(lambda: bj.expected_outstanding(
                os.path.join(self.dir, "fifo"), os.getpid())[1] == 1),
                "the make never joined the pool")
            fd = os.open(os.path.join(self.dir, "fifo"), os.O_WRONLY | os.O_NONBLOCK)
            try:
                os.write(fd, b"++")
            finally:
                os.close(fd)
            self.assertTrue(_wait_for(lambda: _avail(self.dir) == self.TOKENS, timeout=4.0),
                            "a surplus was not drained while the pool was busy")
            self.assertIsNone(p.poll(), "the make finished before the check was meaningful")
        finally:
            self.assertEqual(p.wait(timeout=30), 0)


class StatusCommandTest(DaemonTestBase):
    """`status` (scripts/build_jobserver.py status, what jobserver_ctl.sh
    status's first line is) must report `held` and a freshly-measured
    `clang` count on the SAME line, so an orchestrator heartbeat can compare
    the two at a glance instead of correlating held against a possibly-stale
    samples.tsv column."""

    def test_live_daemon_reports_held_and_clang_on_one_line(self):
        r = subprocess.run([sys.executable, SCRIPT, "status", "--dir", self.dir],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertRegex(r.stdout, r"held~\d+")
        self.assertRegex(r.stdout, r"clang=\d+")
        # Both fields on the SAME line — not one in "held" and the other only
        # reachable via a separate samples.tsv row.
        line = next(l for l in r.stdout.splitlines() if l.startswith("build_jobserver:"))
        self.assertRegex(line, r"held~\d+")
        self.assertRegex(line, r"clang=\d+")

    def test_no_daemon_reports_no_live_daemon_and_fails(self):
        with tempfile.TemporaryDirectory() as empty:
            r = subprocess.run([sys.executable, SCRIPT, "status", "--dir", empty],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("no live daemon", r.stdout)


class LivenessTest(unittest.TestCase):
    def test_alive_is_false_without_a_daemon(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(bj.alive(d))
            rc = subprocess.run([sys.executable, SCRIPT, "alive", "--dir", d]).returncode
            self.assertEqual(rc, 1)

    def test_second_daemon_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            p = subprocess.Popen([sys.executable, SCRIPT, "serve", "--dir", d,
                                  "--tokens", "1", "--interval", "0.2"])
            try:
                self.assertTrue(_wait_for(lambda: bj.alive(d)))
                rc = subprocess.run([sys.executable, SCRIPT, "serve", "--dir", d,
                                     "--tokens", "1"], capture_output=True).returncode
                self.assertEqual(rc, 1)
                self.assertTrue(bj.alive(d), "the refused daemon disturbed the live one")
            finally:
                p.terminate()
                p.wait(timeout=10)
            self.assertFalse(bj.alive(d), "pidfile outlived the daemon")


if __name__ == "__main__":
    unittest.main()
