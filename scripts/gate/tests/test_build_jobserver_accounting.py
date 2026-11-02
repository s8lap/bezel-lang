"""Regression tests for the build token pool's ACCOUNTING
(scripts/build_jobserver.py), lane r38-i1-jobserver.

THE BUG. The daemon used to report how many tokens a joined build held as a
GUESS: `max(0, children - 1)` -- "a make running k children holds k-1
tokens". That is only true while the build runs at its PEAK concurrency. A
build running its LAST job holds ONE token with ONE child, which the guess
read as zero, so the daemon saw a deficit of one, minted a token that was
really held (the pool went to N+1, one compile more than the cap) and then
drained the surplus again. Measured on the production pool: samples.tsv rows
9961-9963 -- `available` 19 -> 21 -> 21, `repaired` +1 then -1, with a compile
in flight (clang=1) the whole time. The same guess was what the status line
reported as "held~", so an operator could not see what the pool was doing:
a wave of 30 launchers read as "launchers=30" with no way to tell the 20
that held a token from the 10 that were QUEUED for one.

THE FIX. Everything the daemon reports is observed:
  * `available`  - the tokens physically in the FIFO (FIONREAD). Ground truth.
  * `held`       - pool size minus that occupancy: the tokens checked OUT.
  * `tok_held`   - launcher processes holding one token each.
  * `tok_wait`   - launcher processes queued for a token.
  * `builds`     - joined make/ninja holders, one implicit job slot each.
  * `jobs`       - the box-wide cap: N + builds.
and the audit re-mints a token only when the shortfall is PROVABLE (nobody
can be holding it), never on the strength of the old guess.

THE PIN. `BEZEL_JOBSERVER_SCRIPT` selects the daemon under test, so the same
tests can be run against the pre-fix daemon to prove they fail without the
fix:

    BEZEL_JOBSERVER_SCRIPT=<pre-fix copy> python3 -m unittest \
        scripts.gate.tests.test_build_jobserver_accounting

Everything runs in a TemporaryDirectory; the production pool is never
touched.
"""

import array
import fcntl
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import termios
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.abspath(os.path.join(HERE, "..", ".."))
# The daemon under test. The pre-fix proof points this at a copy of the old
# script; the default is the one in this tree.
DAEMON = os.environ.get("BEZEL_JOBSERVER_SCRIPT",
                        os.path.join(SCRIPTS, "build_jobserver.py"))
LAUNCHER = os.path.join(SCRIPTS, "build_token.sh")

# A fake JOINED BUILD: it holds the FIFO open, its comm says "make" (so the
# daemon counts it as a build tool) and it has exactly ONE running job. It is
# the shape the old guess got wrong: one child, and the build may well be
# holding a token for it.
_FAKE_JOIN = r'''
import os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR)          # a joined build holds the FIFO
with open("/proc/self/comm", "w") as f:
    f.write("make")
pid = os.fork()
if pid == 0:
    with open("/proc/self/comm", "w") as f:
        f.write("bzl-fake-job")               # its one running job
    time.sleep(300)
    os._exit(0)
time.sleep(300)
'''


def _wait_for(pred, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.05)
    return pred()


class AccountingTestBase(unittest.TestCase):
    TOKENS = 4

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="bezel-js-acct-")
        self.fifo = os.path.join(self.dir, "fifo")
        self.proc = subprocess.Popen(
            [sys.executable, DAEMON, "serve", "--dir", self.dir,
             "--tokens", str(self.TOKENS), "--interval", "0.2", "--confirm", "2"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.assertTrue(_wait_for(lambda: os.path.exists(
            os.path.join(self.dir, "features")) and self._alive()),
            "the daemon never came up")
        self._kids = []

    def tearDown(self):
        for p in self._kids:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        shutil.rmtree(self.dir, ignore_errors=True)

    # -- helpers ---------------------------------------------------------

    def _alive(self):
        r = subprocess.run([sys.executable, DAEMON, "alive", "--dir", self.dir],
                           capture_output=True)
        return r.returncode == 0

    def _avail(self):
        fd = os.open(self.fifo, os.O_RDONLY | os.O_NONBLOCK)
        try:
            arr = array.array("i", [0])
            fcntl.ioctl(fd, termios.FIONREAD, arr)
            return arr[0]
        finally:
            os.close(fd)

    def _take_token(self):
        """Take one token out of the pool and never give it back.

        This models the token that leaves the FIFO: a holder that died, or a
        build that read it and is about to fork. The pool is short by one."""
        fd = os.open(self.fifo, os.O_RDONLY | os.O_NONBLOCK)
        try:
            self.assertEqual(os.read(fd, 1), b"+")
        finally:
            os.close(fd)

    def _fake_join(self):
        """Start a fake joined build: comm "make", holds the FIFO, one job."""
        p = subprocess.Popen([sys.executable, "-c", _FAKE_JOIN, self.fifo],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)
        self._kids.append(p)
        # Wait until the daemon can see it. The pre-fix daemon calls the same
        # census `makes`, so accept either name: the point of this helper is
        # to set up the STATE, not to assert on the format, so that a
        # fail-without run fails on the behavioural assertion.
        def joined():
            f = self._fields()
            return f.get("builds", f.get("makes")) == "1"
        self.assertTrue(_wait_for(joined),
                        "the fake joined build never appeared in the census")
        return p

    def _status(self):
        r = subprocess.run([sys.executable, DAEMON, "status", "--dir", self.dir],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        line = next(l for l in r.stdout.splitlines()
                    if l.startswith("build_jobserver:"))
        return dict(re.findall(r"(\w+)=(\S+)", line)), line

    def _fields(self):
        return self._status()[0]


class ObservedHeldTest(AccountingTestBase):
    """A token that has LEFT the pool is HELD, not lost -- and the status
    line must say so."""

    def test_a_token_in_flight_is_not_read_as_a_lost_token(self):
        # One token leaves the FIFO (a holder took it) and a joined build with
        # ONE running job holds the FIFO. The old guess read that build as
        # holding zero tokens, so the daemon saw a deficit and minted a
        # token that was really held: the pool went to N+1.
        self._take_token()
        self._fake_join()
        # Long enough for several audits (interval 0.2 s, confirm 2).
        time.sleep(2.5)
        self.assertEqual(self._avail(), self.TOKENS - 1,
                         "the daemon minted a token that a holder was holding")

    def test_status_reports_the_observed_held_count(self):
        self._take_token()
        self._fake_join()
        fields, line = self._status()
        self.assertRegex(line, r"held=(\d+)",
                         "the status line must report the OBSERVED held count")
        self.assertEqual(int(fields["held"]), 1,
                         "one token left the pool, so one token is held")
        self.assertEqual(int(fields["available"]), self.TOKENS - 1)
        self.assertNotIn("held~", line,
                         "the guess field must be gone: `held~` was an estimate")


class ProvableDeficitTest(AccountingTestBase):
    """The audit still repairs a token that NOBODY can be holding."""

    def test_a_lost_token_is_restored_when_nobody_holds_it(self):
        self._take_token()          # a holder took it and died
        self.assertTrue(_wait_for(lambda: self._avail() == self.TOKENS, timeout=10),
                        "a provably lost token was never restored")

    def test_a_held_token_is_not_restored(self):
        self._take_token()
        self._fake_join()
        time.sleep(2.5)
        self.assertEqual(self._avail(), self.TOKENS - 1)


class LauncherCensusTest(AccountingTestBase):
    """The launcher split must be VISIBLE: a job that holds a token and a job
    that is queued for one are different things, and the old status line
    reported only their sum."""

    def _launch_wave(self, n):
        env = {k: v for k, v in os.environ.items()
               if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "BEZEL_JOBSERVER",
                            "CMAKE_BUILD_PARALLEL_LEVEL")}
        env.update(BEZEL_JOBSERVER_DIR=self.dir,
                   BEZEL_BUILD_TOKEN_SLOTS_DIR=os.path.join(self.dir, "slots"),
                   BEZEL_BUILD_TOKEN_SLOTS="16",
                   BEZEL_BUILD_TOKEN_POLL="0.3")
        procs = [subprocess.Popen([LAUNCHER, "sleep", "60"], env=env,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL,
                                  start_new_session=True)
                 for _ in range(n)]
        self._kids.extend(procs)
        return procs

    def test_a_wave_drains_the_pool_and_queues_the_excess(self):
        wave = self.TOKENS + 5
        self._launch_wave(wave)
        self.assertTrue(_wait_for(lambda: self._fields().get("tok_wait") == "5",
                                   timeout=20),
                        "the queued launchers never showed up as waiting")
        fields, line = self._status()
        self.assertRegex(line, r"tok_held=(\d+)")
        self.assertRegex(line, r"tok_wait=(\d+)")
        self.assertEqual(int(fields["tok_held"]), self.TOKENS,
                         "every token should be held by a launcher")
        self.assertEqual(int(fields["tok_wait"]), 5,
                         "the excess launchers should be QUEUED, not running")
        self.assertEqual(int(fields["available"]), 0,
                         "the pool must be drained while the wave runs")
        self.assertEqual(int(fields["held"]), self.TOKENS)

    def test_the_pool_is_never_over_full(self):
        self._launch_wave(self.TOKENS + 3)
        self.assertTrue(_wait_for(lambda: self._fields().get("tok_held")
                                  == str(self.TOKENS), timeout=20))
        for _ in range(10):
            self.assertLessEqual(self._avail(), self.TOKENS,
                                 "the pool grew beyond its size")
            time.sleep(0.2)


class BuildCapTest(AccountingTestBase):
    """The 'one implicit job per build' rule must be VISIBLE: the box-wide
    cap is N tokens plus one implicit slot per joined build."""

    def test_the_cap_counts_the_joined_builds(self):
        fields, line = self._status()
        self.assertRegex(line, r"builds=(\d+)")
        self.assertRegex(line, r"jobs=(\d+)")
        self.assertEqual(int(fields["builds"]), 0)
        self.assertEqual(int(fields["jobs"]), self.TOKENS)
        self._fake_join()
        fields, _ = self._status()
        self.assertEqual(int(fields["builds"]), 1)
        self.assertEqual(int(fields["jobs"]), self.TOKENS + 1,
                         "a joined build keeps one implicit job slot")


if __name__ == "__main__":
    unittest.main()
